#!/usr/bin/env python3
# chk64.py -- find 64-bit multiply and divide instructions, which the
# 68060 does not implement.
#
#   chk64.py obj...   fail if any object has one
#   chk64.py -r elf   list the functions that have one, with a count
#   chk64.py -k file...  fail on a 64-bit multiply or divide the supervisor
#                     vector 61 handler can't emulate
#   chk64.py -a obj...   the same, and on movep, cas2, chk2 and cmp2: for
#                     assembler objects, as compiled code keeps switch
#                     tables in .text whose words match those opcodes
import re, subprocess, sys


def sites(path, kernel=False, other=False):
    out = subprocess.run(['m68k-linux-gnu-objdump', '-d', path], check=True,
                         capture_output=True, text=True).stdout
    fn, res = '?', []
    for l in out.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.*)>:', l)
        if m:
            fn = m.group(1)
            continue
        p = l.split('\t')
        if len(p) < 3:
            continue
        if other and re.match(r'(movep|cas2|chk2|cmp2)', p[2]):
            res.append((fn, p[0].strip(), p[2]))
            continue
        if not re.match(r'(mul[su]l|div[su]ll?)', p[2]):
            continue
        w = p[1].split()
        if len(w) < 2 or not int(w[1], 16) >> 10 & 1:
            continue
        # the handler: mul.l with Dn, (An), (d16,An) or #imm
        op = int(w[0], 16)
        if kernel and p[2].startswith('mul') and \
                (op >> 3 & 7 in (0, 2, 5) or op & 0x3f == 0x3c):
            continue
        res.append((fn, p[0].strip(), p[2]))
    return res


if sys.argv[1] == '-r':
    s = sites(sys.argv[2])
    names = sorted(set(f for f, _, _ in s))
    print('64-bit mul/div left for the ISP trap: %d sites in %s' %
          (len(s), ' '.join(names) or '-'))
    sys.exit(0)
other = sys.argv[1] == '-a'
kernel = other or sys.argv[1] == '-k'
bad = 0
for o in sys.argv[1 + kernel:]:
    for f, a, i in sites(o, kernel, other):
        print('%s: %s +%s: %s' % (o, f, a, i))
        bad = 1
if bad:
    sys.exit('[FAIL] 060-unimplemented instruction the kernel cannot emulate'
             if kernel else '[FAIL] 64-bit multiply/divide in an Atari object')
