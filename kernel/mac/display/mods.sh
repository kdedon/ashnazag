#!/bin/sh
set -e
D=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$D/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
DLM=$K/dlm
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
KERNEL=$1 O=$2
mkdir -p "$O/tools" "$O/src/dsuser"
nice -n 19 ${HOSTCC:-cc} -std=gnu89 -O -w -DDLM_TOOL -I"$DLM" -I"$DLM/include" \
 -o "$O/tools/mkksym" "$DLM/tools/mkksym.c" "$DLM/dlm_sym.c"
nice -n 19 ${HOSTCC:-cc} -std=gnu89 -O -w -o "$O/tools/modfix" "$DLM/tools/modfix.c"
"$O/tools/mkksym" -x "$KERNEL" > "$O/exports"
rm -f "$O/mod.d/dsuser"
if ! grep -qw ds_modinit "$O/exports"; then
 echo "[skip] static display kernel"
 exit 0
fi
CPU=-m68040
CHK64=
DF=
if grep -qw ds_vidput "$O/exports"; then
 DF=-DDS_ATARI
 CPU=-m68030
 if grep -qw ata_idcm "$O/exports"; then CPU=-m68000; DF="$DF -DATA060"; CHK64=1; fi
fi
if grep -qw am_dsinit "$O/exports"; then DF=-DDS_AMIGA; fi
objs=
for f in dsdev dsseg dsuser; do
 nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS $CPU $DF -fno-common -Wall -Wno-comment \
  -I"$D" -I"$K/mac/video" -I"$K/mac/adb" -I"$DLM/include" \
  -c "$D/$f.c" -o "$O/src/dsuser/$f.o" 2> "$O/src/dsuser/$f.warn"
 [ ! -s "$O/src/dsuser/$f.warn" ] || { cat "$O/src/dsuser/$f.warn"; exit 1; }
 objs="$objs $O/src/dsuser/$f.o"
done
[ -z "$CHK64" ] || python3 "$K/atari/chk64.py" $objs
m68k-cbm-sysv4-ld -r -o "$O/src/dsuser/Driver.o" $objs
cp "$D/Master" "$O/src/dsuser/"
DLM_TOOLS=$O/tools sh "$DLM/tools/mkmod" -e "$O/exports" -o "$O" "$O/src/dsuser"
python3 "$D/checkmod.py" "$KERNEL" "$O/mod.d/dsuser"
