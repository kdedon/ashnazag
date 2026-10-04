#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
M=$D/../../guest/mod/amigaguest
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT HUP INT TERM
nice -n 19 "${CC:-cc}" -std=c89 -pedantic -Wall -Wextra -Werror \
    -I"$M" "$D/test_amigablit.c" "$M/amigablit.c" -o "$T/blit"
nice -n 19 "$T/blit"
