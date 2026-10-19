#!/usr/bin/env python3
"""tossteady.py -- checks of the TOS environment on a Falcon.

    tossteady.py DESKTOP.png CONSOLE.png CONSOLE-LOG

The desktop's dump has the console's size; tosstat's 10-second samples
while the guest ran, after the desktop came up (the last ones but the
final, which holds the session's end), show no hardware access and
nothing emulated.  Prints [OK] or [FAIL] lines; exit 1 on a failure.
"""
import re
import struct
import sys

HW = ('absent', 'top', 'berr', 'buserr', 'iow', 'mmu', 'illegal', 'isp')
STEADY = 6


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
    # samples while the guest ran: VBLs delivered
    rows = [dict(zip(HW, map(int, r[:7] + r[8:]))) for r in re.findall(
        r'ts-tosstat: absent (\d+) top (\d+) berr (\d+) buserr (\d+) iow (\d+) mmu (\d+) illegal (\d+)\s*'
        r'ts-tosstat: priv \d+ fpriv \d+ ftrap \d+ sys \d+ stop \d+ vbl (\d+) isp (\d+)', log) if int(r[7])]
    last = rows[-STEADY - 1:-1]
    ok = len(last) == STEADY and not any(sum(r.values()) for r in last)
    print('[%s] steady state, %d samples: %s' % ('OK' if ok else 'FAIL', len(last),
          '; '.join(' '.join('%s %d' % (k, r[k]) for k in HW if r[k]) or '0' for r in last)))
    fail |= not ok
    sys.exit(1 if fail else 0)


if __name__ == '__main__':
    main()
