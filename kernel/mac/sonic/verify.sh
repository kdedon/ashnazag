#!/bin/sh
# verify.sh -- static checks of the SONIC driver in a built Mac kernel,
# plus the host tests against the simulated chip.
#
#   sh kernel/mac/sonic/verify.sh [kernel-dir]
#
# Run after build.sh.  Writes only a temporary directory.
set -e

K=$(cd "${1:-$(dirname "$0")/../..}" && pwd)
AUX=$(cd "$K/.." && pwd)
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
HERE="$K/mac/sonic"
ELF="$K/build/unix-mac.elf"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

m68k-elf-nm -S "$ELF" > "$T/nm"
m68k-elf-objdump -d "$ELF" > "$T/dis"
rc=0
python3 - "$ELF" "$T" <<'EOF' || rc=1
import struct, sys
elf, T = sys.argv[1:]
img = open(elf, 'rb').read()
sym, size = {}, {}
for l in open(T + '/nm'):
    p = l.split()
    if len(p) >= 3:
        sym[p[-1]] = int(p[0], 16)
        if len(p) == 4:
            size[p[-1]] = int(p[1], 16)
phoff, = struct.unpack('>I', img[28:32])
phnum, = struct.unpack('>H', img[44:46])
loads = []
for i in range(phnum):
    t, off, va, pa, fs, ms = struct.unpack('>6I', img[phoff + 32*i: phoff + 32*i + 24])
    if t == 1:
        loads.append((va, off, fs))
def L(a):
    for va, off, fs in loads:
        if va <= a and a + 4 <= va + fs:
            return struct.unpack('>I', img[off + a - va: off + a - va + 4])[0]
    raise SystemExit('0x%x not loaded' % a)
dis = open(T + '/dis').read()
def body(n):
    i = dis.find('<%s>:\n' % n)
    return dis[i:dis.find('\n\n', i)] if i >= 0 else ''
bad = 0
def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    bad |= not ok

esz = 13 * 4			# struct cdevsw: 13 longwords
row = [L(sym['cdevsw'] + 18 * esz + o) for o in range(0, esz, 4)]
check(sym['sninfo'] in row, 'cdevsw[18] names sninfo')
check(L(sym['sninfo']) == sym['sn_rinit'] and L(sym['sninfo'] + 4) == sym['sn_winit'],
      'sninfo = {sn_rinit, sn_winit}')
i = dis.find('<p2int>:\n')
p2 = dis[i:dis.find('<p4int>:', i)]
check('ncr96intr' in p2 and 'snintr' in p2 and '#1,%d0' in p2 and
      '%a0@(7680)' in p2, 'p2int: CB2 -> ncr96intr, CA1 + port A bit 0 -> snintr')
check('sn_nslot' in p2, 'p2int counts other slot interrupts')
pool, n = sym['sn_pool'], size.get('sn_pool', 0)
check(n >= 4096 + 4096 + (8 + 32) * 1536 and pool + n < 0x40000000,
      'sn_pool 0x%x, %d bytes, below 1 GB (DTT0: VA = PA, not cached)' % (pool, n))
# the Mac pstart's DTT0: 0-1 GB, supervisor, cache-inhibited (CM = 11)
import re
i = dis.find('<pstart>:\n')
m = re.compile(r'\n[0-9a-f]+ <[^L][^>]*>:\n').search(dis, i + 1)
tt = re.findall(r'movel #(-?\d+),%d0\n[^\n]*movec %d0,%dtt0',
                dis[i:m.start() if m else len(dis)])
check(i >= 0 and tt == [str(0x003FA060)],
      'pstart loads DTT0 = 0x003FA060 (descriptors cache-inhibited)')
i = dis.find('<sn_dcpush>:\n')
d = dis[i:dis.find('<Lidone>:', i)]
check('f468' in d and 'f448' in d, 'sn_dcpush/sn_dcinval use cpushl/cinvl')
sp = body('sn_spl') + body('sn_splx')
check(sp.count('%sr') >= 3, 'sn_spl/sn_splx read and write SR')
check('0x50f0a' in body('sn_init').lower() or '#1357946' in body('sn_init'),
      'sn_init writes SONIC registers at 0x50F0A000')
sys.exit(bad)
EOF

nice -n 19 cc -std=gnu89 -DSN_HOST -w -o "$T/tsonic" "$HERE/test/tsonic.c" \
	"$HERE/test/simsonic.c" "$HERE/sonic.c"
"$T/tsonic" || rc=1
exit $rc
