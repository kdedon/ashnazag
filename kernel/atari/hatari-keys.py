#!/usr/bin/env python3
"""hatari-keys.py -- drive a running Hatari through its command fifo.

    hatari-keys.py FIFO CONSOLE-LOG < script

Script lines:
    wait TEXT     wait until TEXT appears in CONSOLE-LOG after the last match,
                  and after the echo of the last typed line
    type TEXT     type TEXT on the ST keyboard (US layout); \\n is Return
    sleep SECS
    debug CMD     a debugger command, e.g. "screenshot x.png" or "quit 0"
    event EV      an input event, e.g. "keydown 0x1d" or "mousemove 10 5"
Fails (exit 1) when a wait takes longer than WAIT seconds (default 120).
"""
import os, sys, time

LO = "\0\x1b1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0\0\0 "
HI = "\0\x1b!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0\0\0 "
LSHIFT = 42
WAIT = float(os.environ.get('WAIT', '120'))


def scancode(c):
    if c == '\n':
        c = '\r'
    if c != '\0' and c in LO:
        return LO.index(c), False
    if c != '\0' and c in HI:
        return HI.index(c), True
    sys.exit('no key for %r' % c)


def main():
    fifo, log = sys.argv[1], sys.argv[2]
    t0 = time.time()
    while not os.path.exists(fifo):
        if time.time() - t0 > WAIT:
            sys.exit('no fifo %s' % fifo)
        time.sleep(0.2)

    def send(cmd):
        with open(fifo, 'w') as f:
            f.write(cmd + '\n')

    def console():
        try:
            return open(log, 'rb').read().decode('latin-1')
        except OSError:
            return ''

    pos, echo = 0, ''
    for line in sys.stdin:
        line = line.rstrip('\n')
        op, _, arg = line.partition(' ')
        if op == 'wait':
            # skip the echo; a password has none, so give up on it soon
            t0 = time.time()
            while echo and time.time() - t0 < 2:
                i = console().find(echo, pos)
                if i >= 0:
                    pos = i + len(echo)
                    break
                time.sleep(0.2)
            echo = ''
            t0 = time.time()
            while True:
                data = console()
                i = data.find(arg, pos)
                if i >= 0:
                    pos = i + len(arg)
                    break
                if time.time() - t0 > WAIT:
                    sys.exit('timeout waiting for %r' % arg)
                time.sleep(0.2)
        elif op == 'type':
            text = arg.encode().decode('unicode_escape')
            echo = ([l for l in text.split('\n') if l] or [''])[-1]
            for c in text:
                code, shift = scancode(c)
                if shift:
                    send('hatari-event keydown 0x%02x' % LSHIFT)
                send('hatari-event keypress 0x%02x' % code)
                if shift:
                    send('hatari-event keyup 0x%02x' % LSHIFT)
                time.sleep(0.05)
        elif op == 'sleep':
            time.sleep(float(arg))
        elif op == 'debug':
            send('hatari-debug ' + arg)
        elif op == 'event':
            send('hatari-event ' + arg)
        elif op:
            sys.exit('bad line: %s' % line)


main()
