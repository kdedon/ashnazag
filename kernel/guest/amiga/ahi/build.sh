#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
[ "$#" -eq 1 ] || { echo 'usage: build.sh output-directory' >&2; exit 2; }
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
TMP=$(mktemp -d "$OUT/.ahi-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
nice -n 19 "${M68K_AS:-$ROOT/toolchain/bin/m68k-elf-as}" -m68020 \
    "$HERE/container.s" -o "$TMP/container.o"
python3 "$HERE/../rtg/elf2hunk.py" "$TMP/container.o" "$OUT/container.audio"
python3 "$HERE/check.py" "$OUT/container.audio"
