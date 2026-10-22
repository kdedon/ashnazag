#!/bin/sh
# build.sh -- Quadra 800 53C96 SCSI objects for the Mac kernel.
#
#   sh kernel/mac/scsi/build.sh [outdir]
#
# outdir defaults to kernel/build/mac.  Produces macscsi.o (the SCSI glue
# and the partition-map scan) and ncr96drv.o (the 53C96 module's code).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$HERE/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
OUT="${1:-$K/build/mac}"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
mkdir -p "$OUT"

CFLAGS="$AMIX_KERNEL_CFLAGS $AMIX_DIAG_CFLAGS -m68040 -I$AMIX_ROOT/usr/sys/amiga/alien -I$HERE -I$K/dlm/include"
for c in ncr96 ncr96mod sdclust scsimac apm; do
	m68k-cbm-sysv4-gcc $CFLAGS -c "$HERE/$c.c" -o "$OUT/$c.o"
done
m68k-cbm-sysv4-gcc -m68040 -c "$HERE/ncr96pdma.s" -o "$OUT/ncr96pdma.o"
# the 53C96 driver is the ncr96 boot module; a BOOTDIAG kernel links it in
m68k-cbm-sysv4-ld -r -o "$OUT/ncr96drv.o" "$OUT/ncr96.o" "$OUT/ncr96pdma.o" "$OUT/ncr96mod.o" "$OUT/sdclust.o"
if [ -n "$AMIX_DIAG_CFLAGS" ]; then
	m68k-cbm-sysv4-ld -r -o "$OUT/macscsi.o" "$OUT/scsimac.o" "$OUT/apm.o" "$OUT/ncr96drv.o"
else
	m68k-cbm-sysv4-ld -r -o "$OUT/macscsi.o" "$OUT/scsimac.o" "$OUT/apm.o"
fi
echo "[OK] $OUT/macscsi.o $OUT/ncr96drv.o"
