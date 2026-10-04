#!/bin/sh
# mkcoff.sh -- A/UX COFF kernel for A/UX Startup's launch, from the Mac ELF.
#
#   sh kernel/mac/boot/mkcoff.sh [in.elf [out.coff]]
#
# in:  default kernel/build/unix-mac.elf (must contain aux_pstart)
# out: default kernel/build/unix-mac.coff, or kernel/mac/boot/out/unix-mac.coff
#      when kernel/build is not writable.
# The input is converted from a scratch copy; nothing else is written.
set -e

BOOT=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$BOOT/../.." && pwd)
IN="${1:-$K/build/unix-mac.elf}"
if [ -n "$2" ]; then
	OUT="$2"
elif [ -w "$K/build" ]; then
	OUT="$K/build/unix-mac.coff"
else
	mkdir -p "$BOOT/out"
	OUT="$BOOT/out/unix-mac.coff"
fi
[ -f "$IN" ] || { echo "[FAIL] no ELF kernel: $IN"; exit 1; }

make -s -C "$BOOT" >/dev/null

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$IN" "$T/unix.elf"
"$BOOT/build/elf2coff" "$T/unix.elf" "$T/unix.coff"
cp "$T/unix.coff" "$OUT"
echo "[OK] $OUT"
