#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
if [ "${CONFIGURATOR_DIAGNOSTICS:-0}" = 1 ]; then
    CONFIGURATOR_CFLAGS="${CONFIGURATOR_CFLAGS:-} -DOTR_DIAGNOSTICS"
fi
nice -n19 m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 ${CONFIGURATOR_CFLAGS:-} -Wall -Werror -c "$D/configurator.c" -o "$W/configurator.o"
nice -n19 m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 -Wall -Werror -c "$D/admission.c" -o "$W/admission.o"
nice -n19 m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 -Wall -Werror -c "$D/admission-register.c" -o "$W/admission-register.o"
AS=$AUX/toolchain/bin/m68k-elf-as
LD=$AUX/toolchain/bin/m68k-elf-ld
"$AS" -m68020 -o "$W/admission-tail.o" "$D/admission-tail.s"
"$AS" -m68020 -o "$W/admission-hold.o" "$D/admission-hold.s"
"$AS" -m68020 -o "$W/admission-end.o" "$D/admission-end.s"
"$AS" -m68020 -o "$W/aslmrt.o" "$D/../n0n2/mkaslm/aslmrt.s"
python3 "$D/aslm-import.py" cfig_utils 'OTClnt$NtvUtils' 0x100 0x100 \
    OTFindConfigurator=5 OTCfigNumberOfChildren=34 OTCfigGetProviderNameD0=39 OTCfigChangeProviderName=44 OTNewConfiguratorD0=45 > "$W/imports.s"
python3 "$D/aslm-import.py" cfig_client 'OTClnt$NtvClient' 0x100 0x100 \
    OTCreateStateMachineD0=74 OTSMOpenStream=52 OTSMReturnToCaller=55 >> "$W/imports.s"
printf '\t.globl OTFindConfigurator_binding\n' >> "$W/imports.s"
"$AS" -m68020 -o "$W/imports.o" "$W/imports.s"
"$AS" -m68020 -o "$W/hostio.o" "$D/hostio.s"
DIAGNOSTIC=
if [ "${CONFIGURATOR_DIAGNOSTICS:-0}" = 1 ]; then
    "$AS" -m68020 -o "$W/configurator-diag.o" "$D/configurator-diag.s"
    DIAGNOSTIC=$W/configurator-diag.o
fi
"$LD" -r -d -o "$W/hostcfig.o" $DIAGNOSTIC "$W/aslmrt.o" "$W/configurator.o" "$W/admission-tail.o" "$W/admission.o" "$W/admission-register.o" "$W/admission-hold.o" "$W/admission-end.o" "$W/imports.o" "$W/hostio.o"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -o "$W/mkaslm" "$D/../n0n2/mkaslm/mkaslm.c"
"$W/mkaslm" -n 'OTLib$hostcfig' -v 0x100 -i 1 -c aX02 -G 0x224 \
    -I cfig_utils -I cfig_client -e 'OTClnt$auxCfig,0x100,0x100,OTSetupConfigurator' \
    -p 'OTClnt$auxCfig,OTClnt$cfigMkr' -o "$W/AUX Host Configurator Library" "$W/hostcfig.o"
"$W/mkaslm" -t "$W/AUX Host Configurator Library"
"$W/mkaslm" -l "$W/AUX Host Configurator Library"
python3 "$D/test/configurator-parent.py" "$W/mkaslm" "$W/hostcfig.o"
sh "$D/test/pointer-abi.sh" "$W/pointer-abi"
python3 "$D/test/runtime-reloc.py" "$W/aslmrt.o"
