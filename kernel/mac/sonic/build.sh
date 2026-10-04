#!/bin/sh
# build.sh -- Quadra 800 on-board SONIC Ethernet for the Mac kernel.
#
#   sh kernel/mac/sonic/build.sh [outdir]
#
# outdir defaults to kernel/build/mac.  Produces macsonic.o (ld -r of the
# chip core, the DLPI provider and the cache helpers).  -traditional drops
# plain volatile, so the sources use __volatile__ (NOTES.md, Compiler trap).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$HERE/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
OUT="${1:-$K/build/mac}"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
mkdir -p "$OUT"

CFLAGS="$AMIX_KERNEL_CFLAGS $AMIX_DIAG_CFLAGS -m68040 -I$HERE -I$K/otbridge"
for c in sonic sndlpi snvst; do
	nice -n 19 m68k-cbm-sysv4-gcc $CFLAGS -c "$HERE/$c.c" -o "$OUT/sn_$c.o"
done
nice -n 19 m68k-cbm-sysv4-gcc -m68040 -c "$HERE/snsup.s" -o "$OUT/sn_snsup.o"
m68k-cbm-sysv4-ld -r -o "$OUT/macsonic.o" "$OUT/sn_sonic.o" "$OUT/sn_sndlpi.o" "$OUT/sn_snvst.o" \
	"$OUT/sn_snsup.o"
echo "[OK] $OUT/macsonic.o"
