#!/usr/bin/env python3
# Minimal QMP driver: screendumps, keystrokes and monitor commands.
# usage: qmp.py SOCK OUTDIR STEP...
#   wait:N          sleep N seconds
#   shot:NAME       screendump to OUTDIR/NAME.png
#   type:TEXT       send TEXT as key presses ("\n" = Return)
#   serial:TEXT     write TEXT to the serial console socket ("\n" = CR)
#   hmp:CMD         run a monitor command, append output to OUTDIR/monitor.txt
#   quit            stop QEMU
import json, os, socket, struct, sys, threading, time, zlib

KEYS = {' ': 'spc', '\n': 'ret', '-': 'minus', '/': 'slash', '.': 'dot',
        '=': 'equal', ',': 'comma', ';': 'semicolon', "'": 'apostrophe'}
SHIFT = {'|': 'backslash', '>': 'dot', '<': 'comma', '"': 'apostrophe',
         '_': 'minus', ':': 'semicolon', '*': '8', '&': '7', '$': '4'}

class QMP:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
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

def ppm2png(src, dst):
    d = open(src, 'rb').read()
    parts = d.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    px = parts[4]
    raw = b''.join(b'\0' + px[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def keys(q, text):
    for c in text:
        if c.isupper():
            ks = ['shift', c.lower()]
        elif c in SHIFT:
            ks = ['shift', SHIFT[c]]
        else:
            ks = [KEYS.get(c, c)]
        q.cmd('send-key', keys=[{'type': 'qcode', 'data': k} for k in ks],
              **{'hold-time': 80})
        time.sleep(0.25)

def drain(s):
    try:
        while s.recv(4096):
            pass
    except OSError:
        pass

def main():
    sock, out = sys.argv[1], sys.argv[2]
    q = QMP(sock)
    ser = None
    for step in sys.argv[3:]:
        op, _, arg = step.partition(':')
        if op == 'wait':
            time.sleep(float(arg))
        elif op == 'shot':
            ppm = os.path.join(out, arg + '.ppm')
            r = q.cmd('screendump', filename=ppm)
            if 'error' in r:
                print('screendump:', r['error'])
                continue
            time.sleep(0.5)
            ppm2png(ppm, os.path.join(out, arg + '.png'))
            os.unlink(ppm)
        elif op == 'type':
            keys(q, arg.replace('\\n', '\n'))
        elif op == 'serial':
            if ser is None:
                ser = socket.socket(socket.AF_UNIX)
                ser.connect(os.path.join(out, 'serial.sock'))
                # drain the echo: unread single-byte writes fill the socket and stall the guest
                threading.Thread(target=drain, args=(ser,), daemon=True).start()
            for c in arg.replace('\\n', '\r').encode():
                ser.send(bytes([c]))
                time.sleep(0.05)
        elif op == 'hmp':
            r = q.cmd('human-monitor-command', **{'command-line': arg})
            with open(os.path.join(out, 'monitor.txt'), 'a') as f:
                f.write('(qemu) %s\n%s\n' % (arg, r.get('return', r)))
        elif op == 'quit':
            q.cmd('quit')
            return

main()
