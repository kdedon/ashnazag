#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
nice -n19 m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 -Wall -Werror \
    -c "$D/discovery.c" -o "$W/discovery.o"
AS=$AUX/toolchain/bin/m68k-elf-as
LD=$AUX/toolchain/bin/m68k-elf-ld
"$AS" -m68020 -o "$W/aslmrt.o" "$D/../n0n2/mkaslm/aslmrt.s"
"$AUX/toolchain/bin/m68k-elf-objcopy" --redefine-sym __aslm_entry=__aslm_entry_impl "$W/aslmrt.o"
"$AS" -m68020 -o "$W/loadtrace.o" "$D/loadtrace.s"
python3 "$D/test/entry-trace.py" "$W/loadtrace.o"
"$AS" -m68020 -o "$W/hostio.o" "$D/hostio.s"
"$LD" -r -d -o "$W/discovery.o.linked" "$W/loadtrace.o" "$W/aslmrt.o" "$W/discovery.o" "$W/hostio.o"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -o "$W/mkaslm" "$D/../n0n2/mkaslm/mkaslm.c"
"$W/mkaslm" -n 'OTLib$hostinet' -v 0x311 -i 1 -c aX01 \
    -e 'OTModl$auxtcp,0x311,0x311,GetOTauxtcpInstallInfo' \
    -e 'OTModl$auxudp,0x311,0x311,GetOTauxudpInstallInfo' \
    -o "$W/AUX Host Internet Library" "$W/discovery.o.linked"
python3 "$D/test/libr-name.py" "$W/AUX Host Internet Library" 'OTLib$hostinet'
"$W/mkaslm" -t "$W/AUX Host Internet Library"
"$W/mkaslm" -l "$W/AUX Host Internet Library"
python3 "$D/test/discovery.py" "$W"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -Werror -fsanitize=address,undefined \
    -o "$W/getters" "$D/test/discovery.c"
ASAN_OPTIONS=detect_leaks=0 "$W/getters"
