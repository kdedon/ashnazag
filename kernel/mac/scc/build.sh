#!/bin/sh
# build.sh -- compile the Mac SCC tty driver with the AMIX cross compiler.
#
#   sh kernel/mac/scc/build.sh [outdir]
#
# outdir defaults to kernel/build/mac/scc.  Produces scc.o (driver,
# defines sccinfo and sccintr) and scccons.o (console streamtab coinfo).
# Fails on any compiler warning (-Wall; -Wno-comment for the AMIX
# headers), on common symbols, and on sections other than .text, .data,
# .bss and .comment (the AMIX loader binds only those).
set -e

SCC=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$SCC/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

OUT="${1:-$K/build/mac/scc}"
mkdir -p "$OUT"

for f in scc scccons; do
	m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -Wall -Wno-comment \
		-c "$SCC/$f.c" -o "$OUT/$f.o" 2> "$OUT/$f.warn" || {
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
	if m68k-linux-gnu-nm "$OUT/$f.o" | grep -q ' C '; then
		echo "[FAIL] $f.o has common symbols"; exit 1
	fi
	echo "[OK] $OUT/$f.o"
done
