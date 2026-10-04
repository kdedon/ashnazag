#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mod="$here/../../guest/mod/amigaguest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
nice -n 19 "${CC:-cc}" -std=c89 -pedantic -Wall -Wextra -Werror \
    -I"$mod" "$here/test_amigadev.c" "$mod/amigadev.c" -o "$tmp/test_amigadev"
nice -n 19 "$tmp/test_amigadev"
