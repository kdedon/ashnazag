#!/bin/sh
# build.sh -- Quadra 800 53C96 SCSI objects for the Mac kernel.
#
#   sh kernel/mac/scsi/build.sh [outdir]
#
# outdir defaults to kernel/build/mac.  Produces macscsi.o (ld -r of the
# host adapter, the SCSI glue and the partition-map scan).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$HERE/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
OUT="${1:-$K/build/mac}"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
mkdir -p "$OUT"

CFLAGS="$AMIX_KERNEL_CFLAGS $AMIX_DIAG_CFLAGS -m68040 -I$AMIX_ROOT/usr/sys/amiga/alien -I$HERE"
for c in ncr96 scsimac apm; do
	m68k-cbm-sysv4-gcc $CFLAGS -c "$HERE/$c.c" -o "$OUT/$c.o"
done
m68k-cbm-sysv4-gcc -m68040 -c "$HERE/ncr96pdma.s" -o "$OUT/ncr96pdma.o"
m68k-cbm-sysv4-ld -r -o "$OUT/macscsi.o" "$OUT/ncr96.o" "$OUT/scsimac.o" \
	"$OUT/apm.o" "$OUT/ncr96pdma.o"
echo "[OK] $OUT/macscsi.o"
