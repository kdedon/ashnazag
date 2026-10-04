#!/bin/sh
# verify.sh -- static checks of the RTC and restart integration in a built
# kernel, then the host test of rtc.c.
#
#   sh kernel/mac/rtc/verify.sh [kernel-dir]
#
# Run after build.sh.  Reads build/unix-mac.elf; one OK/FAIL line per
# check, exit 1 on any FAIL.
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
    return dis[i:dis.find('\n\n', i)] if i >= 0 else ''
bad = 0
def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    bad |= not ok
def jsr(name, target):
    return ('<%s>' % target) in body(name)

check(all(s in sym for s in ('clkset', 'stime', '__amix_stime', 'mdboot', 'mac_reboot',
      'mac_restart', 'xpram_read', 'xpram_write')), 'RTC and restart symbols present')
check(sym['stime'] != sym['__amix_stime'] and jsr('stime', '__amix_stime') and
      jsr('stime', 'rtc_settime'), 'stime wraps __amix_stime, then sets the RTC')
so = body('__amix_stime')
check('<suser>' in so and '<rf_stime>' in so and '<hrestime>' in so,
      '__amix_stime is the base stime (suser, hrestime, rf_stime)')
st = [L(a) for a in range(sym['sysent'], sym['sysent'] + 0x800, 2)]
check(sym['stime'] in st and sym['__amix_stime'] not in st, 'sysent calls the wrapper')
check(jsr('ufs_mountroot', 'clkset') and jsr('s5mountroot', 'clkset'),
      'ufs_mountroot and s5mountroot call clkset')
check(jsr('clkset', 'rtc_gettime') and jsr('clkset', 'rtc_gmtdelta') and '<hrestime>' in body('clkset'),
      'clkset reads the RTC and the Map offset into hrestime')
check(jsr('uadmin', 'mdboot'), 'uadmin calls mdboot')
md = body('mdboot')
check(md.find('<dhalt>') < md.find('<haltsys>') < md.find('<mac_reboot>') and md.find('<dhalt>') > 0,
      'mdboot: dhalt, haltsys for AD_HALT, else mac_reboot')
check(jsr('mac_reboot', 'mac_restart'), 'mac_reboot ends in mac_restart')
mr = body('mac_restart')
check('movew #9984,%sr' in mr and '<Lmmuoff>' in mr and 'moveal 4 ' in mr and 'jmp %a0@' in mr,
      'mac_restart: IPL 7, translation off, jumps through the reset vector at 4')
mo = body('Lmmuoff')
check(all(x in mo for x in ('cpusha', 'movec %d0,%tc', 'movec %d0,%itt0', 'movec %d0,%dtt0',
      'movec %d0,%cacr', '#1082130432', 'pea %a0@(10)')),
      'Lmmuoff: caches, TC, TT off; reset vectors = ROM + $0A')
ms = body('mac_stop')
check('<Lmmuoff>' in ms and '<fbcons_unlock>' in ms, 'mac_stop: Lmmuoff, then draws and prints')
check(jsr('rtnfirm', 'haltsys'), 'rtnfirm (panic) still halts')
sys.exit(bad)
EOF

TMPDIR="$T" sh "$(dirname "$0")/test/run.sh" > "$T/host.log" || true
grep -E '^(OK|FAIL)' "$T/host.log" | sed 's/^/host: /'
grep -q '^RTC TESTS: all OK$' "$T/host.log"
