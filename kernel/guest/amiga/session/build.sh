#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
[ "$#" -eq 1 ] || { echo 'usage: build.sh output-directory' >&2; exit 2; }
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
TMP=$(mktemp -d "$OUT/.session-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
nice -n 19 "${M68K_AS:-$ROOT/toolchain/bin/m68k-elf-as}" -m68020 \
    "$HERE/session.s" -o "$TMP/session.o"
python3 "$HERE/../dos/elf2hunk.py" "$TMP/session.o" "$OUT/Session"
python3 "$HERE/../dos/check.py" "$OUT/Session"
