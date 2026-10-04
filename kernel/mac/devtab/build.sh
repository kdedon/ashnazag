#!/bin/sh
# build.sh -- Quadra device tables and disk root policy.
#
#   sh kernel/mac/devtab/build.sh [outdir]
#
# Produces outdir/devtab.o (ld -r of macdevsw.o macroot.o macprobe.o
# cfgorig.o).  outdir defaults to kernel/build/mac/devtab.  Fails on
# compiler warnings, common symbols, sections the AMIX loader does not
# bind, BSS in the objects config() reaches (macroot.o, macdevsw.o), and
# superblock offsets that disagree with the kernel headers.
set -e

DT=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$DT/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

OUT="${1:-$K/build/mac/devtab}"
mkdir -p "$OUT"
CC="nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -Wall -Wno-comment -I$DT"

$CC -S "$DT/offchk.c" -o "$OUT/offchk.s" 2> "$OUT/offchk.warn" || {
	cat "$OUT/offchk.warn"; echo "[FAIL] superblock offsets"; exit 1; }
echo "[OK] superblock offsets match the kernel headers"

for f in macdevsw macroot macprobe; do
	$CC -c "$DT/$f.c" -o "$OUT/$f.o" 2> "$OUT/$f.warn" || {
		cat "$OUT/$f.warn"; echo "[FAIL] $f.c"; exit 1; }
	if [ -s "$OUT/$f.warn" ]; then
		cat "$OUT/$f.warn"; echo "[FAIL] $f.c: warnings"; exit 1
	fi
done
nice -n 19 m68k-cbm-sysv4-gcc -m68040 -c "$DT/cfgorig.s" -o "$OUT/cfgorig.o"

for f in macdevsw macroot macprobe cfgorig; do
	bad=$(m68k-linux-gnu-readelf -S "$OUT/$f.o" |
		awk '$1 ~ /^\[/ { n = ($1 == "[") ? $3 : $2;
		     if (n ~ /^\./ && n !~ /^\.(text|data|bss|comment|rela\.text|rela\.data|shstrtab|symtab|strtab)$/) print n }')
	[ -z "$bad" ] || { echo "[FAIL] $f.o has sections: $bad"; exit 1; }
	if m68k-linux-gnu-nm "$OUT/$f.o" | grep -q ' C '; then
		echo "[FAIL] $f.o has common symbols"; exit 1
	fi
done
for f in macdevsw macroot; do
	if m68k-linux-gnu-nm "$OUT/$f.o" | grep -q ' [bB] '; then
		echo "[FAIL] $f.o has BSS (config() runs before BSS is cleared)"; exit 1
	fi
done

nice -n 19 m68k-cbm-sysv4-ld -r -o "$OUT/devtab.o" "$OUT/macdevsw.o" \
	"$OUT/macroot.o" "$OUT/macprobe.o" "$OUT/cfgorig.o"
echo "[OK] $OUT/devtab.o"
