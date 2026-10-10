#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
LIBRARY=${PROVIDER_LIBRARY:-}
[ -n "$LIBRARY" ] || LIBRARY='AUXLib$hostinet'
FILENAME=${PROVIDER_FILENAME:-hostinet.bin}
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
for source in record message provider bootstrap host mps; do
    nice -n19 m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 ${PROVIDER_CFLAGS:-} -Wall -Werror -Wno-comment \
        -c "$D/$source.c" -o "$W/$source.o"
done
AS=$AUX/toolchain/bin/m68k-elf-as
LD=$AUX/toolchain/bin/m68k-elf-ld
"$AS" -m68020 -o "$W/aslmrt.o" "$D/../n0n2/mkaslm/aslmrt.s"
ENTRY=
if [ "${PROVIDER_TRACE_ENTRY:-0}" = 1 ]; then
    "$AUX/toolchain/bin/m68k-elf-objcopy" --redefine-sym __aslm_entry=__aslm_entry_impl "$W/aslmrt.o"
    "$AS" -m68020 -o "$W/loadtrace.o" "$D/loadtrace.s"
    python3 "$D/test/entry-trace.py" "$W/loadtrace.o"
    ENTRY=$W/loadtrace.o
fi
"$AS" -m68020 -o "$W/hostio.o" "$D/hostio.s"
sh "$D/mps-imports.sh" > "$W/imports.s"
"$AS" -m68020 -o "$W/imports.o" "$W/imports.s"
"$LD" -r -d -o "$W/hostinet.o" $ENTRY "$W/aslmrt.o" "$W/record.o" "$W/message.o" \
    "$W/provider.o" "$W/bootstrap.o" "$W/host.o" "$W/mps.o" "$W/hostio.o" "$W/imports.o"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -o "$W/mkaslm" "$D/../n0n2/mkaslm/mkaslm.c"
"$W/mkaslm" -n "$LIBRARY" -v 0x311 -i 1 -c aX01 -I mps_import \
    -e 'OTModl$auxtcp,0x311,0x311,GetOTauxtcpInstallInfo' \
    -e 'OTModl$auxudp,0x311,0x311,GetOTauxudpInstallInfo' \
    -o "$W/$FILENAME" "$W/hostinet.o"
python3 "$D/test/libr-name.py" "$W/$FILENAME" "$LIBRARY"
"$W/mkaslm" -t "$W/$FILENAME"
"$W/mkaslm" -l "$W/$FILENAME"
sh "$D/test/pointer-abi.sh" "$W/pointer-abi"
python3 "$D/test/runtime-reloc.py" "$W/aslmrt.o"
