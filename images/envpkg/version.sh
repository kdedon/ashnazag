#!/bin/sh
# version.sh -- every package's version: the release, then the commit.
D=$(cd "$(dirname "$0")" && pwd)
v=$(cat "$D/VERSION")
c=$(git -C "$D" rev-parse --short=8 HEAD 2> /dev/null) && v=$v-$c
echo "$v"
