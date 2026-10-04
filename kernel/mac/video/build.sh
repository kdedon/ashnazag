#!/bin/sh
# build.sh -- compile the frame-buffer console with the AMIX cross compiler.
#
#   sh kernel/mac/video/build.sh [outdir]
#
# outdir defaults to kernel/build/mac/video.  Produces fbcons.o
# (renderer, VT100 subset), fbprobe.o (mode discovery), fbfont.o and
# fbtty.o (keyboard bytes into the console tty).
# Fails on compiler warnings, on BSS or common symbols (the objects run
# from aux_entry, before BSS is cleared), and on sections the AMIX
# loader does not bind.  With -t also runs the host test (test/run.sh).
set -e

V=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$V/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

TEST=0
if [ "$1" = "-t" ]; then TEST=1; shift; fi
OUT="${1:-$K/build/mac/video}"
mkdir -p "$OUT"

python3 "$V/mkfont.py" --check "$V/fbfont.c" >/dev/null 2>&1 ||
	echo "[--] fbfont.c not checked against the source font (font file missing or differs)"

for f in fbcons fbprobe fbfont fbtty; do
	nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS $AMIX_DIAG_CFLAGS -m68040 -Wall -Wno-comment \
		-c "$V/$f.c" -o "$OUT/$f.o" 2> "$OUT/$f.warn" || {
		cat "$OUT/$f.warn"; echo "[FAIL] $f.c"; exit 1; }
	if [ -s "$OUT/$f.warn" ]; then
		cat "$OUT/$f.warn"; echo "[FAIL] $f.c: warnings"; exit 1
	fi
	bad=$(m68k-linux-gnu-readelf -S "$OUT/$f.o" |
		awk '$1 ~ /^\[/ { n = ($1 == "[") ? $3 : $2;
		     if (n ~ /^\./ && n !~ /^\.(text|data|bss|comment|rela\.text|rela\.data|shstrtab|symtab|strtab)$/) print n }')
	if [ -n "$bad" ]; then
		echo "[FAIL] $f.o has sections: $bad"; exit 1
	fi
	if m68k-linux-gnu-nm "$OUT/$f.o" | grep -q ' [bBC] '; then
		echo "[FAIL] $f.o has BSS/common symbols"; exit 1
	fi
	if [ "$(m68k-linux-gnu-size -A "$OUT/$f.o" | awk '$1==".bss"{print $2}')" != 0 ]; then
		echo "[FAIL] $f.o has a nonempty .bss"; exit 1
	fi
	echo "[OK] $OUT/$f.o"
done

[ "$TEST" -eq 0 ] || sh "$V/test/run.sh"
