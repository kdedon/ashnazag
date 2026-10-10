#!/bin/sh
set -e
A=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$A/.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
O=${1:-$K/build/amiga/test}
mkdir -p "$O"
INC="-I$AMIX_ROOT/usr/include -I$AMIX_ROOT/usr/sys -I$AMIX_ROOT/usr/sys/amiga/inc -I$K/dlm/include -I$K/mac/display -I$K/mac/video -I$K/mac/adb"
for test in startup display; do
 nice -n19 ${HOSTCC:-cc} -std=gnu89 -w -fno-builtin -D_KERNEL -DDS_AMIGA $INC "$A/test/$test.c" -o "$O/$test"
 "$O/$test"
done
