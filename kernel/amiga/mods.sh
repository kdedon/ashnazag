#!/bin/sh
# mods.sh -- the loadable modules for an Amiga (AMIX) kernel.
#
#   sh amiga/mods.sh kernel.elf outdir
#
# Out: outdir/mod.d/{amilib,opci,pci,ne,zz9000,voodoo3,floppy,v3bars}, built for that kernel's CPU; amilib is
# checked against the kernel's exports (it must carry DLM: dlm_cacheflush),
# opci against those and amilib's.  Install them in /etc/conf/mod.d;
# /etc/conf/pci gets openpci.library (the openpci archive's
# Libs/openpci.library) and, optionally, PCI-Configuration.
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
grep -qw dlm_cacheflush "$O/exports" ||
	{ echo "mods.sh: $KERNEL has no DLM" >&2; exit 1; }
CC="$NICE m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -fno-common -Wall -Wno-comment"
CC="$CC -I$DLM/include -I$A/include -I$K/guest/include -I$A/amilib -I$K/atari/netusbee -DNU_PCI"

srcs() {
	case $1 in
	amilib)	echo "$A/amilib/amexec.c $A/amilib/amlibs.c $A/amilib/amhunk.c" \
		    "$A/amilib/amdos.c $A/amilib/ammmu.c $A/amilib/amglue.s" \
		    "$A/amilib/amxplat.c $A/mod/amilib/amilibmod.c" ;;
	opci)	echo "$A/opci/opci.c $A/mod/opci/opcimod.c" ;;
	pci)	echo "$A/pci/pcidev.c" ;;
	zz9000)	echo "$A/zz9000/zz9000.c $A/zz9000/zzds.c $A/mod/zz9000/zz9000mod.c" ;;
	floppy)	echo "$A/floppy/flsvc.c" ;;
	ne)	echo "$K/atari/netusbee/nuchip.c $K/atari/netusbee/nudlpi.c $A/mod/ne/nemod.c" ;;
	voodoo3)	echo "$A/voodoo3/v3ds.c $A/mod/voodoo3/v3mod.c" ;;
	v3bars)	echo "$A/test/v3bars.c" ;;
	esac
}
rm -rf "$O/mod.d"
for m in amilib opci pci ne zz9000 voodoo3 floppy v3bars; do	# each after the module it depends on
	d=$O/src/$m
	rm -rf "$d"; mkdir -p "$d"
	objs=
	for c in $(srcs $m); do
		o=$d/$(basename "$c" | sed 's/\.[cs]$//').o
		cc=$CC
		case $c in */zz9000/zz9000.c) cc=$(echo "$CC" | sed 's/ -Wall//') ;; esac	# upstream, as it is
		$cc -c "$c" -o "$o" 2> "$o.warn" || { cat "$o.warn"; exit 1; }
		if grep -v 'types.h:182\|strsubr.h:70\|In file included\|^ *from ' "$o.warn" | grep .; then
			echo "[FAIL] warnings in $c"; exit 1
		fi
		objs="$objs $o"
	done
	m68k-cbm-sysv4-ld -r -o "$d/Driver.o" $objs
	cp "$A/mod/$m/Master" "$d/"
	[ ! -f "$A/mod/$m/Space.c" ] || cp "$A/mod/$m/Space.c" "$d/"
	DLM_TOOLS=$O/tools sh "$DLM/tools/mkmod" -e "$O/exports" -o "$O" "$d"
done
echo "[OK] $O/mod.d: $(ls "$O/mod.d" | tr '\n' ' ')"
