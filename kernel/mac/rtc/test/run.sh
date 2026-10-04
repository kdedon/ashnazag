#!/bin/sh
# run.sh -- build and run the host test of rtc.c.
set -e
T=$(cd "$(dirname "$0")" && pwd)
O=${TMPDIR:-/tmp}/rtctest.$$
trap 'rm -f "$O"' EXIT
nice -n 19 cc -DRTC_SIM -I"$T" -std=gnu89 -w -o "$O" "$T/rtctest.c" "$T/../rtc.c"
nice -n 19 "$O"
