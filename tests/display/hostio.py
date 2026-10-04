#!/usr/bin/env python3
# hostio.py OUTDIR [SOCKDIR] -- serve the guest's host requests for t_display.
#
# The guest writes "@@ SEQ CMD ARGS" lines on its second serial port
# (SOCKDIR/serb.sock, SOCKDIR defaults to OUTDIR); each gets "@@ok SEQ
# RESULT" or "@@err SEQ WHY".  Input and screen dumps go through a QMP
# socket of their own (SOCKDIR/qmp-ds.sock).  Commands:
#   ping
#   key K[+K...]        press together, release (QEMU qcodes)
#   down K, up K        one transition
#   move DX DY          relative mouse motion
#   button MASK [MS]    mouse buttons (1 = left), answered MS ms later (300)
#   glide N DX DY MS    N moves of DX DY, MS ms apart, answered at once
#   clicks N HOLD GAP   N left clicks, HOLD and GAP in ms
#   shot NAME           screen dump -> display/NAME.ppm, .png; "W H"
#   cmp A B             "same" or "diff N" (pixels)
#   lit NAME            pixels of dump NAME that are not black
#   tos NAME            TOS screen: guest box, menu-bar white, desktop colour
#   ref NAME SHOT DEPTH SEED K
#                       render the dspat.h pattern (table rotated by K)
#                       at SHOT's size, compare: "same" or "diff N"
# Log: OUTDIR/display/hostio.log.  Ends when the guest side closes.
import json, os, socket, struct, sys, threading, time, zlib

OUT = sys.argv[1]
SK = sys.argv[2] if len(sys.argv) > 2 else OUT
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
def macscreen(w, h, px, x0=0, y0=0, x1=None, y1=None):
    # per mille: 50% dither (pixel differs from its right and lower
    # neighbours), white pixels, white pixels in the top 19 rows (menu bar);
    # over the whole screen or the rectangle x0, y0 to x1, y1
    row = 3 * w
    x1 = w if x1 is None else min(x1, w)
    y1 = h if y1 is None else min(y1, h)
    chk = white = top = 0
    for y in range(y0, y1):
        o = y * row
        for x in range(x0, x1):
            p = px[o + 3 * x:o + 3 * x + 3]
            if p == b'\xff\xff\xff':
                white += 1
                if y < 19:
                    top += 1
            if x + 1 < w and y + 1 < h and p != px[o + 3 * x + 3:o + 3 * x + 6] and \
                    p != px[o + row + 3 * x:o + row + 3 * x + 3]:
                chk += 1
    n = max(1, (x1 - x0) * (y1 - y0))
    return 'checker %d white %d top %d' % (1000 * chk // n, 1000 * white // n,
                                           1000 * top // max(1, 19 * (x1 - x0)))


def tosscreen(w, h, px):
    # the guest area (pixels unlike the corner) and, per mille of it:
    # white in its top 16 rows (menu bar), its most common colour below
    # row 30 (desktop), and that colour
    row = 3 * w
    bg = px[0:3]
    xs = [x for x in range(0, w, 4) if any(px[y * row + 3 * x:y * row + 3 * x + 3] != bg
                                           for y in range(0, h, 4))]
    ys = [y for y in range(0, h, 4) if any(px[y * row + 3 * x:y * row + 3 * x + 3] != bg
                                           for x in range(0, w, 4))]
    if not xs or not ys:
        return 'box 0 0 0 0 menu 0 desk 0 000000'
    x0, x1, y0, y1 = xs[0], xs[-1] + 4, ys[0], ys[-1] + 4
    top = sum(1 for y in range(y0, min(y0 + 16, y1)) for x in range(x0, x1)
              if px[y * row + 3 * x:y * row + 3 * x + 3] == b'\xff\xff\xff')
    cnt = {}
    for y in range(y0 + 30, y1, 2):
        for x in range(x0, x1, 2):
            c = px[y * row + 3 * x:y * row + 3 * x + 3]
            cnt[c] = cnt.get(c, 0) + 1
    c, n = max(cnt.items(), key=lambda kv: kv[1]) if cnt else (b'\0\0\0', 0)
    return 'box %d %d %d %d menu %d desk %d %s' % (
        x0, y0, x1 - x0, y1 - y0, 1000 * top // max(1, 16 * (x1 - x0)),
        1000 * n // max(1, sum(cnt.values())), c.hex())


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
    q = QMP(os.path.join(SK, 'qmp-ds.sock'))
    s = connect(os.path.join(SK, 'serb.sock'))
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
    if cmd == 'glide':
        n, dx, dy, gap = int(a[0]), int(a[1]), int(a[2]), int(a[3]) / 1000.

        def go():
            for _ in range(n):
                q.hmp('mouse_move %d %d' % (dx, dy))
                time.sleep(gap)
        threading.Thread(target=go).start()
        return 'started'
    if cmd == 'button':
        q.hmp('mouse_button %d' % int(a[0]))
        time.sleep(int(a[1]) / 1000. if len(a) > 1 else 0.3)
        return 'done'
    if cmd == 'click':
        # n quick clicks: a double click is two
        for _ in range(int(a[0])):
            q.hmp('mouse_button 1')
            time.sleep(0.05)
            q.hmp('mouse_button 0')
            time.sleep(0.1)
        time.sleep(0.3)
        return 'done'
    if cmd == 'clicks':
        # n clicks held hold ms, gap ms apart
        n, hold, gap = int(a[0]), int(a[1]) / 1000., int(a[2]) / 1000.
        for _ in range(n):
            q.hmp('mouse_button 1')
            time.sleep(hold)
            q.hmp('mouse_button 0')
            time.sleep(gap)
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
    if cmd == 'lit':
        w, h, px = readppm(os.path.join(D, os.path.basename(a[0]) + '.ppm'))
        return '%d' % sum(1 for i in range(0, len(px), 3) if px[i:i + 3] != b'\0\0\0')
    if cmd == 'mac':
        w, h, px = readppm(os.path.join(D, os.path.basename(a[0]) + '.ppm'))
        return macscreen(w, h, px, *[int(v) for v in a[1:5]])
    if cmd == 'tos':
        w, h, px = readppm(os.path.join(D, os.path.basename(a[0]) + '.ppm'))
        return tosscreen(w, h, px)
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
