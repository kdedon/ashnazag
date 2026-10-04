#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
if [ "$#" -ne 1 ]; then
    echo 'usage: build.sh output-directory' >&2
    exit 2
fi
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
TMP=$(mktemp -d "$OUT/.rtg-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -Wall -Wextra -Werror "$HERE/abi.c" -o "$TMP/abi"
"$TMP/abi" > "$TMP/rtgabi.inc"
nice -n 19 "${M68K_AS:-$ROOT/toolchain/bin/m68k-elf-as}" -m68020 \
    -I "$HERE" -I "$TMP" "$HERE/container.s" -o "$TMP/container.o"
python3 "$HERE/elf2hunk.py" "$TMP/container.o" "$OUT/container.card"
python3 "$HERE/check.py" "$OUT/container.card"
