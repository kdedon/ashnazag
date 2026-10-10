#!/bin/sh
set -e
T=$(cd "$(dirname "$0")" && pwd)
[ -n "$1" ] || { echo "usage: gmem-check.sh output-directory"; exit 1; }
mkdir -p "$1"
O=$(cd "$1" && pwd)
for test in t_gmem t_gmemseg; do
 nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -Wno-unused-function -Wno-unused-variable \
  -I"$T/gmem-host" -I"$T/../include" "$T/$test.c" -o "$O/$test"
 "$O/$test"
done
