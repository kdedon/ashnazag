#!/usr/bin/env python3
"""tospass.py -- checks of starttos -P -v on a Falcon.

    tospass.py DESKTOP.png CONSOLE.png CONSOLE-LOG

The desktop's dump has the console's size (TOS took the console's Videl
mode); between two of starttos's mouse reports GEM's pointer moved as
far as the mouse did; the log has a redraw time.  Prints [OK] or [FAIL]
lines; exit 1 on a failure.
"""
import re
import struct
import sys


def size(path):
    with open(path, 'rb') as f:
        return struct.unpack('>II', f.read(24)[16:24])


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    fail = 0
    d, c = size(sys.argv[1]), size(sys.argv[2])
    ok = d == c
    print('[%s] desktop %dx%d, console %dx%d' % ('OK' if ok else 'FAIL', d[0], d[1], c[0], c[1]))
    fail |= not ok
    log = open(sys.argv[3], 'rb').read().decode('latin-1')
    m = [tuple(map(int, r)) for r in
         re.findall(r'starttos: mouse at (-?\d+),(-?\d+), sent (-?\d+),(-?\d+)', log)]
    ok = len(m) >= 2 and all(b[0] - a[0] == b[2] - a[2] and b[1] - a[1] == b[3] - a[3]
                             for a, b in zip(m, m[1:]))
    print('[%s] mouse moves arrive whole: %s' % ('OK' if ok else 'FAIL',
          ' '.join('%d,%d' % (r[0], r[1]) for r in m) or 'no reports'))
    fail |= not ok
    t = re.findall(r'starttos: redraw (\d+) ms', log)
    print('[%s] redraw %s ms' % ('OK' if t else 'FAIL', ', '.join(t) or 'not timed'))
    fail |= not t
    sys.exit(1 if fail else 0)


if __name__ == '__main__':
    main()
