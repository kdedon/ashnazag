#!/bin/sh
# build.sh -- the Quadra RTC, PRAM and restart object for the Mac kernel.
#
#   sh kernel/mac/rtc/build.sh [outdir]
#
# outdir defaults to kernel/build/mac/rtc.  Produces rtc.o.  Fails on
# warnings, common symbols and sections other than .text, .data, .bss and
# .comment.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$HERE/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

OUT="${1:-$K/build/mac/rtc}"
mkdir -p "$OUT"
nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -Wall \
	-c "$HERE/rtc.c" -o "$OUT/rtc.o" 2> "$OUT/rtc.warn" || {
	cat "$OUT/rtc.warn"; echo "[FAIL] rtc.c"; exit 1; }
if [ -s "$OUT/rtc.warn" ]; then
	cat "$OUT/rtc.warn"; echo "[FAIL] rtc.c: warnings"; exit 1
fi
bad=$(m68k-linux-gnu-readelf -S "$OUT/rtc.o" |
	awk '$1 ~ /^\[/ { n = ($1 == "[") ? $3 : $2;
	     if (n ~ /^\./ && n !~ /^\.(text|data|bss|comment|rela\.text|rela\.data|shstrtab|symtab|strtab)$/) print n }')
[ -z "$bad" ] || { echo "[FAIL] rtc.o has sections: $bad"; exit 1; }
if m68k-linux-gnu-nm "$OUT/rtc.o" | grep -q ' C '; then
	echo "[FAIL] rtc.o has common symbols"; exit 1
fi
echo "[OK] $OUT/rtc.o"
