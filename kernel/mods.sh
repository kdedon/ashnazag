#!/bin/sh
# mods.sh -- the loadable modules for one target.
#
#   sh kernel/mods.sh TARGET=mac|amiga|atari030|atari060 OUT [KERNEL=elf] [set...]
#   sh kernel/mods.sh -t kernel.elf	(prints the kernel's target)
#
# Sets: guest (environment modules), display (dsuser), drivers (the
# target's hardware modules); default all.  Out: OUT/mod.d/*, plus
# OUT/modadmin for the Atari drivers.
#
# Undefined symbols are checked against include/kabi.<target>, the kernel
# interface modules may use.  With KERNEL, that list is checked against
# the kernel's exports, and the checks that need the kernel run too.
set -e

K=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$K/.." && pwd)
DLM=$K/dlm
G=$K/guest
NICE="nice -n 19"
HCC="$NICE ${HOSTCC:-cc} -std=gnu89 -O -w"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"

if [ "$1" = -t ]; then
	nm=$(m68k-elf-nm "$2" | awk '{print $NF}')
	sym() { printf '%s\n' "$nm" | grep -qx "$1"; }
	if sym am_dsinit; then echo amiga
	elif sym ata_idcm; then echo atari060
	elif sym ds_vidput; then echo atari030
	else echo mac
	fi
	exit 0
fi

. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
export PATH
usage() { echo "usage: mods.sh TARGET=mac|amiga|atari030|atari060 OUT [KERNEL=elf] [guest|display|drivers]..." >&2; exit 2; }
case $1 in TARGET=*) TARGET=${1#TARGET=} ;; *) usage ;; esac
[ -n "$2" ] || usage
O=$2; shift 2
KERNEL=
case $1 in KERNEL=*) KERNEL=${1#KERNEL=}; shift ;; esac
SETS=${*:-guest display drivers}

# target: guest flags, display CPU and flags, driver CPU and flags, 64-bit check
case $TARGET in
mac)	GF= DCPU=-m68040 DF= VCC= CHK64= ;;
amiga)	GF=-DDS_AMIGA DCPU=-m68040 DF=-DDS_AMIGA VCC= CHK64= ;;
atari030)	GF=-DDS_ATARI DCPU=-m68030 DF=-DDS_ATARI VCC=-m68030 CHK64= ;;
atari060)	GF="-DDS_ATARI -DATA060" DCPU=-m68000 DF="-DDS_ATARI -DATA060"
	VCC="-m68040 -DATA060" CHK64=1 ;;
*)	usage ;;
esac
KABI=$K/include/kabi.$TARGET

# modules built on include/ alone, without the AMIX headers
OWN=" amilib auxexec auxsnd cpmguest dmasnd floppy opci pci radeon radeonds scsi tdfx v3bars voodoo3 "
kcc() {	# module
	case $OWN in
	*" $1 "*) echo "$NICE env AMIX_SYSROOT=$O/none m68k-cbm-sysv4-gcc -O -traditional -fno-builtin" \
		"-Dinline=__inline__ -m68020 -D_KERNEL -DSVR40 -DSVR4 -I$K/include" ;;
	*) echo "$NICE m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS" ;;
	esac
}

mkdir -p "$O/tools" "$O/src"
$HCC -DDLM_TOOL -I"$DLM" -I"$DLM/include" -o "$O/tools/mkksym" \
	"$DLM/tools/mkksym.c" "$DLM/dlm_sym.c"
$HCC -o "$O/tools/modfix" "$DLM/tools/modfix.c"
if [ -n "$KERNEL" ]; then
	"$O/tools/mkksym" -x "$KERNEL" | sort -u > "$O/exports"
	if ! grep -qx ds_modinit "$O/exports"; then
		echo "[skip] static display kernel"
		SETS=$(echo $SETS | sed 's/display//')
		[ -n "$(echo $SETS)" ] || exit 0
	fi
	if sort -u "$KABI" | comm -23 - "$O/exports" | grep .; then
		echo "[FAIL] $KABI names symbols $KERNEL does not export"; exit 1
	fi
fi

# the compiler's warnings, less the system headers' own, fail the build
warn() {	# src obj
	if grep -v 'types.h:182\|strsubr.h:70\|In file included\|^ *from ' "$2.warn" | grep .; then
		echo "[FAIL] warnings in $1"; exit 1
	fi
}
mkmod() {	# dir
	DLM_TOOLS=$O/tools sh "$DLM/tools/mkmod" -e "$KABI" -o "$O" "$1"
}

guest() {
	for m in guestcore auxcore auxexec uinter tosguest amigaguest cpmguest tiplatform nextguest; do
		KCC="$(kcc $m) -Wall -I$DLM/include -I$G/include -I$G/mod/auxcore -I$G/mod/guestcore -I$K/mac/display -I$G/mod/tosguest -I$G/voyage200 $GF"
		d=$O/src/$m
		rm -rf "$d"; mkdir -p "$d"
		objs=
		for c in "$G/mod/$m"/*.c; do
			o=$d/$(basename "$c" .c).o
			$KCC -c "$c" -o "$o" 2> "$o.warn" || { cat "$o.warn"; exit 1; }
			warn "$c" "$o"
			objs="$objs $o"
		done
		for s in "$G/mod/$m"/*.s; do
			[ -f "$s" ] || continue
			$NICE m68k-cbm-sysv4-gcc -m68040 -c "$s" -o "$d/$(basename "$s" .s).o"
			objs="$objs $d/$(basename "$s" .s).o"
		done
		m68k-cbm-sysv4-ld -r -o "$d/Driver.o" $objs
		cp "$G/mod/$m/Master" "$d/"
		mkmod "$d"
		[ "$m" != guestcore ] || python3 "$G/chkgatemod.py" "$O/mod.d/$m"
	done
	[ -z "$KERNEL" ] ||
		python3 "$G/chklonemod.py" "$KERNEL" "$O/mod.d/guestcore" "$O/mod.d/cpmguest" "$O/mod.d/tosguest"
}

display() {
	D=$K/mac/display
	d=$O/src/dsuser
	rm -rf "$d"; mkdir -p "$d"
	objs=
	for f in dsdev dsseg dsuser; do
		$NICE m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS $DCPU $DF -fno-common -Wall -Wno-comment \
		 -I"$D" -I"$K/mac/video" -I"$K/mac/adb" -I"$DLM/include" \
		 -c "$D/$f.c" -o "$d/$f.o" 2> "$d/$f.warn"
		[ ! -s "$d/$f.warn" ] || { cat "$d/$f.warn"; exit 1; }
		objs="$objs $d/$f.o"
	done
	[ -z "$CHK64" ] || python3 "$K/atari/chk64.py" $objs
	m68k-cbm-sysv4-ld -r -o "$d/Driver.o" $objs
	cp "$D/Master" "$d/"
	mkmod "$d"
	[ -z "$KERNEL" ] || python3 "$D/checkmod.py" "$KERNEL" "$O/mod.d/dsuser"
}

atarisrcs() {
	A=$K/atari
	case $1 in
	scsi)	echo "$A/scsi/scsi.c $A/scsi/falcon.c $A/scsi/tt.c $A/mod/scsi/scsimod.c" ;;
	sd)	echo "$A/scsi/sd.c" ;;
	aen)	echo "$A/netusbee/nuchip.c $A/netusbee/nudlpi.c $A/mod/aen/aenmod.c" ;;
	dpn)	echo "$A/dayna/dpchip.c $A/dayna/dpdlpi.c $A/mod/dpn/dpnmod.c" ;;
	dmasnd)	echo "$A/sound/dmasnd.c $A/mod/dmasnd/dmasndmod.c" ;;
	auxsnd)	echo "$K/mac/sound/auxsnd.c $K/mac/sound/sndmod.c" ;;
	esac
}
atari() {
	A=$K/atari
	for m in scsi sd aen dpn dmasnd auxsnd; do
		CC="$(kcc $m) $VCC -Wall -Wno-comment -I$DLM/include"
		[ $m != sd ] || CC="$CC -I$AMIX_ROOT/usr/sys/amiga/alien"
		d=$O/src/$m
		rm -rf "$d"; mkdir -p "$d"
		objs=
		for c in $(atarisrcs $m); do
			o=$d/$(basename "$c" .c).o
			$CC -I"$(dirname "$c")" -I"$A/dayna" -I"$A/netusbee" -I"$A/scsi" -I"$A" \
				-DDS_ATARI -I"$K/mac/display" -I"$K/mac/sound" -c "$c" -o "$o" 2> "$o.warn" ||
				{ cat "$o.warn"; exit 1; }
			warn "$c" "$o"
			objs="$objs $o"
		done
		[ -z "$CHK64" ] || python3 "$A/chk64.py" $objs
		m68k-cbm-sysv4-ld -r -o "$d/Driver.o" $objs
		M=$A/mod/$m/Master
		[ -f "$M" ] || M=$K/mac/sound/mod/$m/Master
		cp "$M" "$d/"
		mkmod "$d"
	done
	# modadmin, which registers them at boot
	sh "$DLM/tools/modadmin.sh" "$O/src"
	cp "$O/src/modadmin" "$O/modadmin"
}

amigasrcs() {
	A=$K/amiga
	case $1 in
	amilib)	echo "$A/amilib/amexec.c $A/amilib/amlibs.c $A/amilib/amhunk.c" \
		    "$A/amilib/amdos.c $A/amilib/ammmu.c $A/amilib/amglue.s" \
		    "$A/amilib/amxplat.c $A/mod/amilib/amilibmod.c" ;;
	opci)	echo "$A/opci/opci.c $A/mod/opci/opcimod.c" ;;
	pci)	echo "$A/pci/pcidev.c" ;;
	zz9000)	echo "$A/zz9000/zz9000.c $A/zz9000/zzds.c $A/mod/zz9000/zz9000mod.c" ;;
	floppy)	echo "$A/floppy/flsvc.c" ;;
	ne|xsurf)	echo "$K/atari/netusbee/nuchip.c $K/atari/netusbee/nudlpi.c $A/mod/ne/nemod.c" ;;
	voodoo3)	echo "$A/voodoo3/v3ds.c $A/mod/voodoo3/v3mod.c" ;;
	tdfx)	echo "$K/drm/tdfx_drv.c $A/mod/tdfx/tdfxmod.c" ;;
	radeonds)	echo "$A/radeon/rds.c $A/radeon/rdsinit.c $A/mod/radeonds/rdsmod.c" ;;
	radeon)	echo "$K/drm/radeon_drv.c $K/drm/radeon_cp.c $K/drm/radeon_state.c $K/drm/radeon_irq.c" \
		    "$K/drm/radeon_mem.c $A/mod/radeon/radeonmod.c" ;;
	v3bars)	echo "$A/test/v3bars.c" ;;
	esac
}
amiga() {
	A=$K/amiga
	[ -z "$KERNEL" ] || grep -qx dlm_cacheflush "$O/exports" ||
		{ echo "mods.sh: $KERNEL has no DLM" >&2; exit 1; }
	for m in amilib opci pci ne xsurf zz9000 voodoo3 tdfx radeonds radeon floppy v3bars; do	# each after the module it depends on
		CC="$(kcc $m) -fno-common -Wall -Wno-comment"
		CC="$CC -I$DLM/include -I$A/include -I$K/drm -I$K/guest/include -I$A/amilib -I$K/atari/netusbee -DNU_PCI"
		d=$O/src/$m
		rm -rf "$d"; mkdir -p "$d"
		objs=
		for c in $(amigasrcs $m); do
			o=$d/$(basename "$c" | sed 's/\.[cs]$//').o
			cc=$CC
			case $c in */zz9000/zz9000.c) cc=$(echo "$CC" | sed 's/ -Wall//') ;; esac	# upstream, as it is
			[ $m != xsurf ] || cc=$(echo "$CC" | sed 's/ -DNU_PCI/ -DNU_XSURF/')
			# ANSI templates, upstream warnings kept; __STDC__ 0 keeps the system headers' extensions
			case $c in */drm/*) cc="$(echo "$CC" | sed 's/ -traditional//; s/ -Wall//') -D__STDC__=0 -DDS_AMIGA -I$K/mac/display" ;; esac
			$cc -c "$c" -o "$o" 2> "$o.warn" || { cat "$o.warn"; exit 1; }
			warn "$c" "$o"
			objs="$objs $o"
		done
		m68k-cbm-sysv4-ld -r -o "$d/Driver.o" $objs
		python3 "$K/atari/chk64.py" -k "$d/Driver.o"
		cp "$A/mod/$m/Master" "$d/"
		[ ! -f "$A/mod/$m/Space.c" ] || cp "$A/mod/$m/Space.c" "$d/"
		mkmod "$d"
	done
}

rm -rf "$O/mod.d"
for s in $SETS; do
	case $s in
	guest|display) $s ;;
	drivers)
		case $TARGET in
		amiga) amiga ;;
		atari*) atari ;;
		esac ;;
	*) usage ;;
	esac
done
echo "[OK] $O/mod.d: $(ls "$O/mod.d" 2>/dev/null | tr '\n' ' ')"
