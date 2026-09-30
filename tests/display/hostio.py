#!/usr/bin/env python3
# hostio.py OUTDIR -- serve the guest's host requests for t_display.
#
# The guest writes "@@ SEQ CMD ARGS" lines on its second serial port
# (OUTDIR/serb.sock); each gets "@@ok SEQ RESULT" or "@@err SEQ WHY".
# Input and screen dumps go through a QMP socket of their own
# (OUTDIR/qmp-ds.sock).  Commands:
#   ping
#   key K[+K...]        press together, release (QEMU qcodes)
#   down K, up K        one transition
#   move DX DY          relative mouse motion
#   button MASK         mouse buttons (1 = left)
#   shot NAME           screen dump -> display/NAME.ppm, .png; "W H"
#   cmp A B             "same" or "diff N" (pixels)
#   ref NAME SHOT DEPTH SEED K
#                       render the dspat.h pattern (table rotated by K)
#                       at SHOT's size, compare: "same" or "diff N"
# Log: OUTDIR/display/hostio.log.  Ends when the guest side closes.
import json, os, socket, struct, sys, time, zlib

OUT = sys.argv[1]
D = os.path.join(OUT, 'display')
os.makedirs(D, exist_ok=True)
LOG = open(os.path.join(D, 'hostio.log'), 'a', buffering=1)


def connect(path, tmo=120):
    t0 = time.time()
    while time.time() - t0 < tmo:
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(path)
            return s
        except OSError:
            time.sleep(0.2)
    sys.exit('hostio: no %s' % path)


class QMP:
    def __init__(self, path):
        self.s = connect(path)
        self.f = self.s.makefile('rw')
        self.f.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def hmp(self, line):
        return self.cmd('human-monitor-command', **{'command-line': line})


def readppm(path):
    d = open(path, 'rb').read()
    p = d.split(maxsplit=4)
    return int(p[1]), int(p[2]), p[4]


def writeppm(path, w, h, px):
    open(path, 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + px)


def writepng(path, w, h, px):
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' +
                           chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def diff(a, b):
    if len(a) != len(b):
        return -1
    return sum(1 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3])


# dspat.h, in Python
def pattern(w, h, depth, seed, k):
    n = 1 << depth if depth <= 8 else 256
    pal = [bytes(((37 * i + seed) & 255, (91 * i + 40) & 255, (255 - 7 * i) & 255))
           for i in range(n)]
    if depth <= 8:      # hardware entry i holds palette entry (i + k) % n
        hw = [pal[(i + k) % n] for i in range(n)]
    else:
        hw = pal
    if depth == 16:
        hw = [bytes(((c[0] >> 3) * 255 // 31, (c[1] >> 3) * 255 // 31, (c[2] >> 3) * 255 // 31))
              for c in hw]
    rows = []
    for y in range(h):
        base = 3 * (y >> 4) + seed
        blocks = [hw[(bx + base) % n] for bx in range((w + 15) >> 4)]
        row = b''.join(c * 16 for c in blocks)
        rows.append(row[:w * 3])
    return b''.join(rows)


def main():
    q = QMP(os.path.join(OUT, 'qmp-ds.sock'))
    s = connect(os.path.join(OUT, 'serb.sock'))
    buf = b''
    while True:
        c = s.recv(4096)
        if not c:
            break
        buf += c
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            line = line.decode('latin-1').strip('\r\0 ')
            i = line.find('@@ ')
            if i < 0:
                continue
            a = line[i + 3:].split()
            if len(a) < 2:
                continue
            seq, cmd, arg = a[0], a[1], a[2:]
            try:
                r = serve(q, cmd, arg)
                rep = '@@ok %s %s' % (seq, r)
            except Exception as e:
                rep = '@@err %s %s' % (seq, str(e).replace('\n', ' '))
            LOG.write('%.3f %s -> %s\n' % (time.time(), line[i:], rep))
            s.sendall(rep.encode() + b'\r\n')


def serve(q, cmd, a):
    if cmd == 'ping':
        return 'pong'
    if cmd == 'key':
        r = q.cmd('send-key', keys=[{'type': 'qcode', 'data': k} for k in a[0].split('+')],
                  **{'hold-time': 100})
        time.sleep(0.3)
        return 'error' in r and r['error']['desc'] or 'done'
    if cmd in ('down', 'up'):
        r = q.cmd('input-send-event', events=[{'type': 'key', 'data': {
            'down': cmd == 'down', 'key': {'type': 'qcode', 'data': a[0]}}}])
        time.sleep(0.2)
        return 'error' in r and r['error']['desc'] or 'done'
    if cmd == 'move':
        q.hmp('mouse_move %d %d' % (int(a[0]), int(a[1])))
        time.sleep(0.3)
        return 'done'
    if cmd == 'button':
        q.hmp('mouse_button %d' % int(a[0]))
        time.sleep(0.3)
        return 'done'
    if cmd == 'shot':
        name = os.path.basename(a[0])
        ppm = os.path.join(D, name + '.ppm')
        if os.path.exists(ppm):
            os.unlink(ppm)
        r = q.cmd('screendump', filename=ppm)
        if 'error' in r:
            raise Exception(r['error']['desc'])
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 16:
                break
            time.sleep(0.1)
        time.sleep(0.2)
        w, h, px = readppm(ppm)
        writepng(os.path.join(D, name + '.png'), w, h, px)
        return '%d %d' % (w, h)
    if cmd == 'cmp':
        w1, h1, p1 = readppm(os.path.join(D, os.path.basename(a[0]) + '.ppm'))
        w2, h2, p2 = readppm(os.path.join(D, os.path.basename(a[1]) + '.ppm'))
        n = diff(p1, p2) if (w1, h1) == (w2, h2) else -1
        return 'same' if n == 0 else 'diff %d' % n
    if cmd == 'ref':
        name, shot = os.path.basename(a[0]), os.path.basename(a[1])
        depth, seed, k = int(a[2]), int(a[3]), int(a[4])
        w, h, px = readppm(os.path.join(D, shot + '.ppm'))
        ref = pattern(w, h, depth, seed, k)
        writeppm(os.path.join(D, name + '.ppm'), w, h, ref)
        writepng(os.path.join(D, name + '.png'), w, h, ref)
        n = diff(px, ref)
        return 'same' if n == 0 else 'diff %d' % n
    raise Exception('unknown command ' + cmd)


main()
