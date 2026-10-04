#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
G=$(CDPATH= cd -- "$HERE/../../guest/amiga" && pwd)
O=$(mktemp -d)
trap 'rm -rf "$O"' EXIT HUP INT TERM
mkdir "$O/root"
${CC:-cc} -std=c89 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I"$G" "$HERE/hostfs.c" "$G/hostfs.c" -o "$O/test"
"$O/test" "$O/root"
