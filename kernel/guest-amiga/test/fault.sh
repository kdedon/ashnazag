#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mod="$here/../../guest/mod/amigaguest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
nice -n 19 "${CC:-cc}" -std=gnu89 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$mod" "$here/test_amigafault.c" "$mod/amigadev.c" -o "$tmp/test_amigafault"
nice -n 19 "$tmp/test_amigafault"
