#!/bin/sh
# verify.sh -- static checks of the frame-buffer console in the linked kernel.
#
#   sh kernel/mac/video/verify.sh [kernel-dir]
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
m68k-elf-readelf -lW "$ELF" > "$T/phdr"

python3 - "$ELF" "$T" "$K" <<'EOF'
import re, struct, sys
elf, T, K = sys.argv[1:]
img = open(elf, 'rb').read()
sym, typ = {}, {}
for l in open(T + '/nm'):
    p = l.split()
    if len(p) == 3:
        sym[p[2]] = int(p[0], 16)
        typ[p[2]] = p[1]
phoff, = struct.unpack('>I', img[28:32])
phnum, = struct.unpack('>H', img[44:46])
loads = []
for i in range(phnum):
    t, off, va, pa, fs, ms = struct.unpack('>6I', img[phoff + 32*i: phoff + 32*i + 24])
    if t == 1:
        loads.append((va, off, fs))
def rd(a, n):
    for va, off, fs in loads:
        if va <= a and a + n <= va + fs:
            return img[off + a - va: off + a - va + n]
    raise SystemExit('address 0x%x not in a PT_LOAD' % a)
dis = open(T + '/dis').read()
ins = []            # (address, text) of every instruction
for l in dis.split('\n'):
    m = re.match(r' +([0-9a-f]+):\t', l)
    if m:
        ins.append((int(m.group(1), 16), l))
addrs = sorted(set(v for k, v in sym.items() if typ[k] in 'TtWw'))
def body(name):
    """instructions from name up to the next text symbol (labels share addresses)"""
    if name not in sym:
        return ''
    a = sym[name]
    nxt = [x for x in addrs if x > a]
    e = nxt[0] if nxt else a + 0x10000
    if name in ('aux_entry', 'pstart'):         # local labels inside
        e = a + 0x800
    return '\n'.join(t for x, t in ins if a <= x < e)
def calls(name):
    return re.findall(r'jsr (?:%pc@\()?[0-9a-f]+ <([A-Za-z_0-9]+)>', body(name))
bad = 0
def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    if not ok:
        bad = 1

# state is initialised data (used from aux_entry, before BSS is cleared)
for s in ('fbcons', 'fbvt_kern', 'fbvt_tty', 'fbprobe', 'fb_font'):
    check(typ.get(s) in ('D', 'd'), '%s is initialised data (%s)' % (s, typ.get(s)))
check(struct.unpack('>I', rd(sym['fbcons'], 4))[0] == 0, 'fbcons.fc_on starts 0')
check(rd(sym['fbvt_kern'] + 4 + 4 + 4 * 8 + 4 + 8 + 4, 4) == b'\0\0\0\1',
      'kernel parser: LF implies CR (v_kern = 1)')

# font in the image = fbfont.c
src = open(K + '/mac/video/fbfont.c').read()
rows = re.findall(r'\{ ((?:0x[0-9a-f]{2}, ){15}0x[0-9a-f]{2}) \}', src)
font = bytes(int(x, 16) for r in rows for x in r.split(', '))
check(len(font) == 4096 and rd(sym['fb_font'], 4096) == font, 'fb_font is fbfont.c byte for byte')

# hooks, in order
c = calls('aux_entry')
check('fbcons_auxinit' in c and c.index('mac_scc_init') < c.index('fbcons_auxinit') < c.index('mac_puts'),
      'aux_entry: mac_scc_init, fbcons_auxinit, then the first message')
j = re.findall(r'\tjsr ([^\n]*)', body('mac_shim_main'))
check(j and j[0].endswith('<fbcons_biinit>'), 'mac_shim_main: fbcons_biinit is the first call')
c = calls('putchar')
check(c[-1:] == ['fbcons_kputc'] and 'scc_putc' in c, 'putchar: SCC, then fbcons_kputc')
check('fbcons_report' in calls('config'), 'config calls fbcons_report')
w = calls('sccwsrv')
check('fbcons_active' in w and 'scc_conout' in w and 'fbcons_write' in calls('scc_conout'),
      'sccwsrv: console output drawn by scc_conout (fbcons_write), not paced by the SCC')
check('fbcons_write' not in calls('scc_txfill'), 'scc_txfill (SCC-only path) does not draw')
check('fbcons_unlock' in calls('mac_stop'), 'mac_stop draws what an interrupted owner queued')
check('scc_conin' in calls('fbcons_input') and 'qenable' in calls('scc_conin'),
      'fbcons_input -> scc_conin -> qenable')
check('fbcons_input' in calls('fb_answer'), 'terminal answers go through fbcons_input')

# the frame buffer is reached VA = PA: DTT1 = 0x807FA060 in pstart
p = body('pstart')
check(re.search(r'movel #-2139119520,%d0\n[^\n]*movec %d0,%dtt1', p) is not None,
      'pstart: DTT1 0x807FA060 (S-only, cache-inhibited, 0x80000000-0xFFFFFFFF)')

# nothing else writes the DAFB: the only 0xF98xxxxx constant is the sense read
fb = ''.join(body(f) for f in ('fp_sense', 'fp_lowmem', 'fp_rom', 'fp_olddepth', 'dr_vp',
    'dr_find', 'dr_ptr', 'fbcons_auxinit', 'fbcons_biinit'))
check('#-109051876' in body('fp_sense') and '0xf98' not in fb.lower(),
      'fbprobe touches only DAFB 0xF980001C, by a read')
w = [l for l in dis.split('\n') if re.search(r'mov[a-z.]* [^,]*,(0xf98|-10905)', l)]
check(not w, 'no store to 0xF98xxxxx anywhere (%d)' % len(w))
sys.exit(bad)
EOF

# host renderer test
sh "$K/mac/video/test/run.sh"
