#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$D/../../.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
nice -n19 python3 "$D/test/imports.py" "$W/imports"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -Wextra \
    -fsanitize=address,undefined -o "$W/record" "$D/record.c" "$D/test/record.c"
ASAN_OPTIONS=detect_leaks=0 nice -n19 "$W/record"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -Wextra \
    -fsanitize=address,undefined -o "$W/message" "$D/record.c" "$D/message.c" "$D/test/message.c"
ASAN_OPTIONS=detect_leaks=0 nice -n19 "$W/message"
sh "$D/mps-imports.sh" > "$W/mps.s"
"$AUX/toolchain/linux/bin/m68k-linux-gnu-as" -m68020 -o "$W/mps.o" "$W/mps.s"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
nice -n19 m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 -Wall \
    -c "$D/record.c" -o "$W/record.o"
nice -n19 m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 -Wall \
    -c "$D/message.c" -o "$W/message.o"
echo 'PASS AMIX 68020 record/message builds and ABI guards'
sh "$D/test/configurator-host.sh" "$W/configurator-tests"
