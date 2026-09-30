#!/usr/bin/env python3
# chktab.py -- static checks of the A/UX call table and signal maps.
#
#   chktab.py auxcalls.c auxconv.c
#
# - numbers unique and in 1..169, trap #0 argument counts equal A/UX
#   sysent's (decoded from the A/UX kernel's table);
# - class N rows name an AMIX call that exists (not a nosys slot);
# - the signal maps are inverse permutations of 1..31.
import re, sys

# A/UX sysent argument counts, 1..169 (0 = nosys rows omitted)
AUXARGC = dict(enumerate(map(int, '''
0 1 0 3 3 3 1 1 2 2 1 2 1 1 3 2 3 1 2 3 0 0 1 1 0 1 4 1 2 0 2 2 2 2 1 0 0 2 5 3 0
1 0 1 4 1 1 0 2 6 5 1 4 5 3 4 3 3 0 3 1 1 3 2 1 1 2 1 0 0 3 3 3 0 2 3 3 5 2 4 6 3
5 4 3 6 1 2 2 2 5 2 3 4 0 0 0 0 0 0 2 2 2 2 0 2 3 3 2 1 4 2 2 3 2 2 1 2 2 0 0 1 2
2 2 2 2 0 1 3 1 1 1 2 2 3 1 1 2 4 2 4 1 3 3 2 0 2 0 1 0 3 2 5 5 0 2 1 3 3 3 3 2 2
2 2 2 4 2 2'''.split())))
AMIX_NOSYS = {0, 56, 82, 83, 105, 140} | set(range(64, 78))

bad = 0


def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    bad |= not ok


src = open(sys.argv[1]).read()
rows = re.findall(r'\{ (-?\d+), (\d+), (\w+), (\d+), (\w+) \}', src)
nums = [int(r[0]) for r in rows if int(r[0]) >= 0]
check(len(nums) == len(set(nums)) and all(1 <= n <= 169 for n in nums),
      '%d rows, numbers unique in 1..169' % len(nums))
wrong = [n for n, a in ((int(r[0]), int(r[1])) for r in rows) if n >= 0 and AUXARGC.get(n) != a]
check(not wrong, 'argument counts match A/UX sysent' + (': %s' % wrong if wrong else ''))
nrows = [(int(r[0]), int(r[3])) for r in rows if int(r[0]) >= 0 and r[4] == '0' and r[2] != 'T']
badn = [n for n, m in nrows if m == 0 or m in AMIX_NOSYS or m > 141]
check(not badn, '%d class N rows name live AMIX calls' % len(nrows) + (': %s' % badn if badn else ''))

conv = open(sys.argv[2]).read()
def arr(name):
    m = re.search(r'%s\[32\] = \{([^}]*)\}' % name, conv)
    return [int(x) for x in re.sub(r'/\*.*?\*/', '', m.group(1)).split(',')]
si, so = arr('sigin'), arr('sigout')
check(sorted(si[1:]) == list(range(1, 32)) and sorted(so[1:]) == list(range(1, 32)),
      'signal maps are permutations of 1..31')
check(all(so[si[s]] == s for s in range(1, 32)), 'sigout inverts sigin')
sys.exit(bad)
