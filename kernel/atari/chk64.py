#!/usr/bin/env python3
# chk64.py -- find 64-bit multiply and divide instructions, which the
# 68060 does not implement.
#
#   chk64.py obj...   fail if any object has one
#   chk64.py -r elf   list the functions that have one, with a count
import re, subprocess, sys


def sites(path):
    out = subprocess.run(['m68k-linux-gnu-objdump', '-d', path], check=True,
                         capture_output=True, text=True).stdout
    fn, res = '?', []
    for l in out.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.*)>:', l)
        if m:
            fn = m.group(1)
            continue
        p = l.split('\t')
        if len(p) < 3 or not re.match(r'(mul[su]l|div[su]ll?)', p[2]):
            continue
        w = p[1].split()
        if len(w) >= 2 and int(w[1], 16) >> 10 & 1:
            res.append((fn, p[0].strip(), p[2]))
    return res


if sys.argv[1] == '-r':
    s = sites(sys.argv[2])
    names = sorted(set(f for f, _, _ in s))
    print('64-bit mul/div left for the ISP trap: %d sites in %s' %
          (len(s), ' '.join(names) or '-'))
    sys.exit(0)
bad = 0
for o in sys.argv[1:]:
    for f, a, i in sites(o):
        print('%s: %s +%s: %s' % (o, f, a, i))
        bad = 1
if bad:
    sys.exit('[FAIL] 64-bit multiply/divide in an Atari object')
