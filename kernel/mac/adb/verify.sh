#!/bin/sh
# verify.sh -- static checks of the ADB integration in a built kernel.
#
#   sh kernel/mac/adb/verify.sh [kernel-dir]
#
# Run after build.sh (and mac/integrate/verify.sh).  Reads
# build/unix-mac.elf; one OK/FAIL line per check, exit 1 on any FAIL.
set -e

K=$(cd "${1:-$(dirname "$0")/../..}" && pwd)
AUX=$(cd "$K/.." && pwd)
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
ELF="$K/build/unix-mac.elf"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

m68k-elf-nm "$ELF" > "$T/nm"
m68k-elf-objdump -d "$ELF" > "$T/dis"

python3 - "$ELF" "$T" <<'EOF'
import struct, sys
elf, T = sys.argv[1:]
img = open(elf, 'rb').read()
sym = {}
for l in open(T + '/nm'):
    p = l.split()
    if len(p) == 3:
        sym[p[2]] = int(p[0], 16)
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
def body(name):
    i = dis.find('<%s>:\n' % name)
    return dis[i:dis.find('\n\n', i)]
bad = 0
def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    bad |= not ok

p1 = body('p1int')
i2, i6 = p1.find('btst #2,%d0'), p1.find('btst #6,%d0')
check(0 <= i2 < i6, 'p1int tests VIA1 SR (bit 2) before T1 (bit 6)')
check('adb_tick' in p1 and p1.find('movew #8704,%sr') < p1.find('adb_tick') < p1.find('clock_int'),
      'tick path calls adb_tick at IPL 2, before clock_int')
ad = body('Lp1adb')
check(ad.find('movew #9216,%sr') < ad.find('adb_intr') < ad.find('movew #8448,%sr') <
      ad.find('adb_soft') < ad.find('intret'), 'Lp1adb: IPL 4 adb_intr, IPL 1 adb_soft, intret')
check(L(sym['io_init']) == sym['parinit'], 'io_init[0] = parinit')
check('adb_init' in body('parinit'), 'parinit calls adb_init')
ti = L(sym['adb_ttyin'])
if 'fbcons_input' in sym:
    check(ti == sym['fbcons_input'], 'adb_ttyin = fbcons_input (keyboard -> console tty)')
else:
    check(ti == 0, 'adb_ttyin = 0 (no frame-buffer console linked: keyboard has no tty sink)')
sp = body('adb_spl')
check('movew %sr,' in sp and ',%sr' in sp.split('movew %sr,')[1], 'adb_spl reads and raises SR (not dropped by -traditional)')
sys.exit(bad)
EOF
