#!/bin/sh
# run.sh -- build and run the host tests of the ADB driver.
set -e
T=$(cd "$(dirname "$0")" && pwd)
O=${TMPDIR:-/tmp}/adbtest.$$
trap 'rm -f "$O"' EXIT
nice -n 19 cc -DADB_HOST -std=gnu89 -Wall -Wno-implicit-int -Wno-implicit-function-declaration \
	-Wno-old-style-definition -Wno-unused-function -Wno-strict-prototypes -Wno-deprecated-non-prototype \
	-o "$O" "$T/test.c" "$T/sim.c" "$T/../adb.c" "$T/../adbkbd.c" "$T/../adbms.c" 2>&1 |
	grep -v 'unknown warning option' || true
nice -n 19 "$O"
