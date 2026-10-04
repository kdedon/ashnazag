#!/bin/sh
# build.sh -- compile the Mac ADB driver with the AMIX cross compiler.
#
#   sh kernel/mac/adb/build.sh [outdir]
#
# outdir defaults to kernel/build/mac/adb.  Produces adb.o (adb.c,
# adbkbd.c, adbms.c linked -r).  When some Mac source defines
# fbcons_input() (the console-tty input hook of the frame-buffer
# console), keyboard bytes go there; otherwise adb_ttyin stays 0 and the
# keyboard reaches only a raw consumer.  ADB_FBCONS=1/0 overrides.
# Fails on warnings, common symbols and sections other than .text,
# .data, .bss and .comment.
set -e

ADB=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$ADB/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

OUT="${1:-$K/build/mac/adb}"
mkdir -p "$OUT"
DEF=""
if [ -z "$ADB_FBCONS" ]; then
	ADB_FBCONS=0
	for f in "$K"/mac/*.c "$K"/mac/*/*.c; do
		case "$f" in */adb/*) continue ;; esac
		grep -q '^fbcons_input(' "$f" 2>/dev/null && ADB_FBCONS=1
	done
fi
[ "$ADB_FBCONS" = 1 ] && DEF="-DADB_FBCONS"

for f in adb adbkbd adbms; do
	nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS $DEF -m68040 -Wall -Wno-comment \
		-c "$ADB/$f.c" -o "$OUT/$f.o" 2> "$OUT/$f.warn" || {
		cat "$OUT/$f.warn"; echo "[FAIL] $f.c"; exit 1; }
	if [ -s "$OUT/$f.warn" ]; then
		cat "$OUT/$f.warn"; echo "[FAIL] $f.c: warnings"; exit 1
	fi
done
nice -n 19 m68k-cbm-sysv4-ld -r -o "$OUT/adbdrv.o" "$OUT/adb.o" "$OUT/adbkbd.o" "$OUT/adbms.o"
mv "$OUT/adbdrv.o" "$OUT/adb.o"
rm -f "$OUT/adbkbd.o" "$OUT/adbms.o"

bad=$(m68k-linux-gnu-readelf -S "$OUT/adb.o" |
	awk '$1 ~ /^\[/ { n = ($1 == "[") ? $3 : $2;
	     if (n ~ /^\./ && n !~ /^\.(text|data|bss|comment|rela\.text|rela\.data|shstrtab|symtab|strtab)$/) print n }')
[ -z "$bad" ] || { echo "[FAIL] adb.o has sections: $bad"; exit 1; }
if m68k-linux-gnu-nm "$OUT/adb.o" | grep -q ' C '; then
	echo "[FAIL] adb.o has common symbols"; exit 1
fi
echo "[OK] $OUT/adb.o ${DEF:-(no frame-buffer console)}"
