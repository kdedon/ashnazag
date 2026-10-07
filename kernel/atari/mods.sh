#!/bin/sh
# mods.sh -- the optional-hardware driver modules for an Atari kernel.
#
#   sh atari/mods.sh kernel.elf outdir
#
# Out: outdir/mod.d/{scsi,sd,aen,dpn,dmasnd,auxsnd}, built for that kernel's
# CPU and checked against its exports.  They go in /etc/conf/mod.d; sysinit
# registers aen (18), dmasnd (46), auxsnd (47) and dpn (58), which load on
# their first open; sd (over scsi) loads on the first open of a card 1 disk.
set -e

A=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$A/.." && pwd)
AUX=$(cd "$K/.." && pwd)
DLM=$K/dlm
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
[ $# -eq 2 ] || { echo "usage: mods.sh kernel.elf outdir" >&2; exit 2; }
KERNEL=$1 O=$2
NICE="nice -n 19"
HCC="$NICE ${HOSTCC:-cc} -std=gnu89 -O -w"

mkdir -p "$O/tools" "$O/src"
$HCC -DDLM_TOOL -I"$DLM" -I"$DLM/include" -o "$O/tools/mkksym" \
	"$DLM/tools/mkksym.c" "$DLM/dlm_sym.c"
$HCC -o "$O/tools/modfix" "$DLM/tools/modfix.c"
"$O/tools/mkksym" -x "$KERNEL" > "$O/exports"
if grep -qw ata_idcm "$O/exports"; then
	CC="m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -DATA060"
	CHK64=1
else
	CC="m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68030"
	CHK64=
fi
CC="$NICE $CC -Wall -Wno-comment -I$DLM/include"

srcs() {
	case $1 in
	scsi)	echo "$A/scsi/scsi.c $A/scsi/falcon.c $A/scsi/tt.c $A/mod/scsi/scsimod.c" ;;
	sd)	echo "$A/scsi/sd.c" ;;
	aen)	echo "$A/netusbee/nuchip.c $A/netusbee/nudlpi.c $A/mod/aen/aenmod.c" ;;
	dpn)	echo "$A/dayna/dpchip.c $A/dayna/dpdlpi.c $A/mod/dpn/dpnmod.c" ;;
	dmasnd)	echo "$A/sound/dmasnd.c $A/mod/dmasnd/dmasndmod.c" ;;
	auxsnd)	echo "$K/mac/sound/auxsnd.c $K/mac/sound/sndmod.c" ;;
	esac
}
rm -rf "$O/mod.d"
for m in scsi sd aen dpn dmasnd auxsnd; do
	d=$O/src/$m
	rm -rf "$d"; mkdir -p "$d"
	objs=
	for c in $(srcs $m); do
		o=$d/$(basename "$c" .c).o
		$CC -I"$(dirname "$c")" -I"$A/dayna" -I"$A/netusbee" -I"$A/scsi" -I"$A" \
			-I"$AMIX_ROOT/usr/sys/amiga/alien" -DDS_ATARI -I"$K/mac/display" -I"$K/mac/sound" -c "$c" -o "$o" 2> "$o.warn" ||
			{ cat "$o.warn"; exit 1; }
		if grep -v 'types.h:182\|strsubr.h:70\|In file included\|^ *from ' "$o.warn" | grep .; then
			echo "[FAIL] warnings in $c"; exit 1
		fi
		objs="$objs $o"
	done
	[ -z "$CHK64" ] || python3 "$A/chk64.py" $objs
	m68k-cbm-sysv4-ld -r -o "$d/Driver.o" $objs
	M=$A/mod/$m/Master
	[ -f "$M" ] || M=$K/mac/sound/mod/$m/Master
	cp "$M" "$d/"
	DLM_TOOLS=$O/tools sh "$DLM/tools/mkmod" -e "$O/exports" -o "$O" "$d"
done

# modadmin, which registers them at boot
sh "$DLM/tools/modadmin.sh" "$O/src"
cp "$O/src/modadmin" "$O/modadmin"
echo "[OK] $O/mod.d: $(ls "$O/mod.d" | tr '\n' ' ')"
