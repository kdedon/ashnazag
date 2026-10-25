#!/usr/bin/env python3
"""hatari-keys.py -- drive a running Hatari through its command fifo.

    hatari-keys.py FIFO CONSOLE-LOG < script

Script lines:
    wait TEXT     wait until TEXT appears in CONSOLE-LOG after the last match,
                  and after the echo of the last typed line
    typeconsole TEXT  type one console command, acknowledging every character
    waitjob       require the last command echo, background job, and shell prompt
    type TEXT     type TEXT on the ST keyboard (US layout); \\n is Return
    sleep SECS
    debug CMD     a debugger command, e.g. "screenshot x.png" or "quit 0"
    event EV      an input event, e.g. "keydown 0x1d" or "mousemove 10 5"
    cmd CMD       a control command, e.g. "hatari-shortcut recsound"
    shot FILE MODE [REF]
                  screenshot FILE every few seconds until the screen is ready, then
                  go on (after the wait limit too, so the check reports it):
                  xterm = focused shell prompt in the default terminal;
                  desktop = a GEM desktop; arrange = Cubase's Arrange window;
                  same = like REF; greeter = not like
                  REF, not blank, and unchanged for 3 shots
Fails (exit 1) when a wait takes longer than WAIT seconds (default 120).
"""
import os, re, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test'))
import struct, zlib
import tosshot

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


# The default terminal's first cell and solid cursor identify a focused shell.
PROMPTS = (
    ('......', '......', '......', '.#.#..', '.#.#..', '#####.', '.#.#..', '#####.', '.#.#..', '.#.#..', '......', '......', '......'),
    ('......', '......', '..#...', '.####.', '#.#...', '#.#...', '.###..', '..#.#.', '..#.#.', '####..', '..#...', '......', '......'),
)


def terminal_prompt(rows):
    if len(rows) < 78 or len(rows[65]) < 38:
        return False
    black, white = (0, 0, 0), (255, 255, 255)
    glyph = tuple(''.join('#' if p == black else '.' if p == white else '?' for p in rows[y][20:26]) for y in range(65, 78))
    return glyph in PROMPTS and all(p == black for y in range(65, 78) for p in rows[y][32:38])


def ready(path, mode, ref, last):
    """Whether the screenshot PATH shows the wanted screen; LAST is the shot before."""
    try:
        if mode == 'xterm':
            return terminal_prompt(tosshot.load(path)[2]) and last is not None and terminal_prompt(tosshot.load(last)[2])
        if mode == 'desktop':
            return tosshot.desktop(path) is None
        if mode == 'arrange':
            return tosshot.arrange(path) is None
        if mode == 'same':
            return tosshot.diff(path, ref) < 0.01
        w, h, rows = tosshot.load(path)
        busy = sum(1 for r in rows for p in r if p != rows[0][0])
        return (busy > w * h // 100 and tosshot.diff(path, ref) > 0.2
                and last is not None and tosshot.diff(path, last) == 0)
    except (SystemExit, OSError, ValueError, zlib.error, struct.error):
        return False    # still being written


def job_end(data, echo, pos):
    i = data.find(echo + '\r\n', pos)
    if i < 0:
        return None
    m = re.match(r'(?:\[\d+\]\s+)?\d+\r\n[#$] ', data[i + len(echo) + 2:])
    return None if m is None else i + len(echo) + 2 + m.end()


def type_console(text, send, console, limit=WAIT):
    if '\r' in text or '\n' in text.rstrip('\n') or text.count('\n') > 1:
        raise ValueError('typeconsole accepts one command')
    start = len(console())
    expected = ''
    deadline = time.monotonic() + limit
    for n, c in enumerate(text):
        if time.monotonic() >= deadline:
            raise TimeoutError('console command deadline exceeded')
        code, shift = scancode(c)
        if shift:
            send('hatari-event keydown 0x%02x' % LSHIFT)
        send('hatari-event keypress 0x%02x' % code)
        if shift:
            send('hatari-event keyup 0x%02x' % LSHIFT)
        expected += '\r\n' if c == '\n' else c
        while console().find(expected, start) < 0:
            if time.monotonic() >= deadline:
                raise TimeoutError('console input not acknowledged at character %d (%r)' % (n + 1, c))
            time.sleep(0.01)


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

    pos, echo, jobpos = 0, '', 0
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
        elif op == 'waitjob':
            if not echo:
                sys.exit('waitjob requires a typed command')
            t0 = time.time()
            while True:
                end = job_end(console(), echo, jobpos)
                if end is not None:
                    pos, echo = end, ''
                    break
                if time.time() - t0 > WAIT:
                    sys.exit('background launch not acknowledged: command echo, job or prompt missing')
                time.sleep(0.2)
        elif op in ('type', 'typeconsole'):
            jobpos = len(console())
            text = arg.encode().decode('unicode_escape')
            echo = ([l for l in text.split('\n') if l] or [''])[-1]
            if op == 'typeconsole':
                try:
                    type_console(text, send, console)
                except (TimeoutError, ValueError) as error:
                    sys.exit(str(error))
                continue
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
        elif op == 'shot':
            f = arg.split()
            name, mode, ref = f[0], f[1], (f + [None])[2]
            t0, last, n = time.time(), None, 0
            while time.time() - t0 < WAIT:
                n += 1
                cur = '%s-%d.png' % (name[:-4], n % 2)
                if os.path.exists(cur):
                    os.remove(cur)
                send('hatari-debug screenshot ' + cur)
                time.sleep(5)
                if ready(cur, mode, ref, last):
                    break
                last = cur
            else:
                if mode == 'xterm':
                    sys.exit('xterm shell prompt/focus not reached')
                sys.stderr.write('shot %s: %s not reached\n' % (name, mode))
            if os.path.exists(cur):
                os.replace(cur, name)
            for x in ('%s-%d.png' % (name[:-4], i) for i in (0, 1)):
                if os.path.exists(x):
                    os.remove(x)
        elif op == 'event':
            send('hatari-event ' + arg)
        elif op == 'cmd':
            send(arg)
        elif op:
            sys.exit('bad line: %s' % line)


if __name__ == '__main__':
    main()
