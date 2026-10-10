#!/bin/sh
set -eu
O=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
for t in anchor config; do
 nice -n19 cc -std=gnu89 -O1 -g -fsanitize=address,undefined -I"$O" \
  "$O/test/$t.c" "$O/anchor.c" "$O/config.c" -o "$OUT/$t-test"
 ASAN_OPTIONS=detect_leaks=0 "$OUT/$t-test"
done
