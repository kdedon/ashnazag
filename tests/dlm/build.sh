#!/bin/sh
# build.sh -- loadable modules for t_dlm, checked against a kernel image.
#
#   sh tests/dlm/build.sh kernel.elf outdir
#
# Out: outdir/mod.d/<module>.  A kernel without the module system gets
# no modules (exit 0), and t_dlm then skips.
#
#   dlmta   exports dlmta_value, dlmta_add; prints on load
#   dlmtb   depends on dlmta, calls dlmta_add(234), reads lbolt
#   dlmtd   depends on a missing module            EINVAL
#   dlmtl   _load returns ENODEV                   ENODEV
#   dlmtu   undefined symbol                       ERELOC
#   dlmtv   wrapper revision 2                     EBADVER
#   dlmtw   wrapper word 0xfffffff0 (32-bit wrap)  ERELOC
set -e

T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
DLM=$KDIR/dlm
KERNEL=$1
OUT=$2
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/bin:$PATH"
export PATH

rm -rf "$OUT"
if ! m68k-elf-nm "$KERNEL" | grep -q ' T dlm_init$'; then
	echo "[skip] $KERNEL has no loadable-module support"
	exit 0
fi
mkdir -p "$OUT/tools" "$OUT/src"
HCC="nice -n 19 ${HOSTCC:-cc} -std=gnu89 -O -w"
$HCC -DDLM_TOOL -I"$DLM" -I"$DLM/include" -o "$OUT/tools/mkksym" \
	"$DLM/tools/mkksym.c" "$DLM/dlm_sym.c"
$HCC -o "$OUT/tools/modfix" "$DLM/tools/modfix.c"
"$OUT/tools/mkksym" -x "$KERNEL" > "$OUT/exports"
# lets dlmtu past mkmod's check; the kernel still lacks the name
{ cat "$OUT/exports"; echo dlm_no_such_symbol; } > "$OUT/exports.u"

CC="nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -I$DLM/include"
mk() {	# module [exports]
	d=$OUT/src/$1
	mkdir -p "$d"
	cp "$T/dlm/mod/$1/Master" "$d/"
	$CC -c "$T/dlm/mod/$1/$1.c" -o "$d/Driver.o"
	DLM_TOOLS=$OUT/tools sh "$DLM/tools/mkmod" -e "${2:-$OUT/exports}" -o "$OUT" "$d"
}
mk dlmta
mk dlmtb
mk dlmtd
mk dlmtl
mk dlmtv
mk dlmtu "$OUT/exports.u"

# dlmtw by hand: mkmod refuses a word 0 without its relocation
d=$OUT/src/dlmtw
mkdir -p "$d"
printf '\t.data\n\t.globl\tdlmtw_wrapper\ndlmtw_wrapper:\n\t.long\t1,0,0,0,0,0\n' > "$d/w.s"
printf '\t.section .moddata,"aw",@progbits\n\t.balign\t4\n\t.long\t0xfffffff0\n' >> "$d/w.s"
m68k-cbm-sysv4-as -o "$d/w.o" "$d/w.s"
m68k-cbm-sysv4-ld -r -o "$OUT/mod.d/dlmtw" "$d/w.o"
python3 - "$OUT/mod.d/dlmtw" <<'EOF'
import struct, sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
u32 = lambda o: struct.unpack('>I', d[o:o+4])[0]
shoff, shnum, shstr = u32(32), struct.unpack('>H', d[48:50])[0], struct.unpack('>H', d[50:52])[0]
so = u32(shoff + 40 * shstr + 16)
for i in range(shnum):
    h = shoff + 40 * i
    if d[so + u32(h):].startswith(b'.moddata\0'):
        d[h + 4:h + 8] = struct.pack('>I', 13)
open(p, 'wb').write(d)
EOF
ls "$OUT/mod.d"
