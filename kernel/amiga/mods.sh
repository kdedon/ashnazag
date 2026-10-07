#!/bin/sh
# mods.sh -- the loadable modules for an Amiga (AMIX) kernel.
#
#   sh amiga/mods.sh kernel.elf outdir
#
# Out: outdir/mod.d/{amilib,opci}, built for that kernel's CPU; amilib is
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
CC="$NICE m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -Wall -Wno-comment"
CC="$CC -I$DLM/include -I$A/include -I$A/amilib"

srcs() {
	case $1 in
	amilib)	echo "$A/amilib/amexec.c $A/amilib/amlibs.c $A/amilib/amhunk.c" \
		    "$A/amilib/amdos.c $A/amilib/ammmu.c $A/amilib/amglue.s" \
		    "$A/amilib/amxplat.c $A/mod/amilib/amilibmod.c" ;;
	opci)	echo "$A/opci/opci.c $A/mod/opci/opcimod.c" ;;
	esac
}
rm -rf "$O/mod.d"
for m in amilib opci; do	# amilib first: opci depends on it
	d=$O/src/$m
	rm -rf "$d"; mkdir -p "$d"
	objs=
	for c in $(srcs $m); do
		o=$d/$(basename "$c" | sed 's/\.[cs]$//').o
		$CC -c "$c" -o "$o" 2> "$o.warn" || { cat "$o.warn"; exit 1; }
		if grep -v 'types.h:182\|strsubr.h:70\|In file included\|^ *from ' "$o.warn" | grep .; then
			echo "[FAIL] warnings in $c"; exit 1
		fi
		objs="$objs $o"
	done
	m68k-cbm-sysv4-ld -r -o "$d/Driver.o" $objs
	cp "$A/mod/$m/Master" "$A/mod/$m/Space.c" "$d/"
	DLM_TOOLS=$O/tools sh "$DLM/tools/mkmod" -e "$O/exports" -o "$O" "$d"
done
echo "[OK] $O/mod.d: $(ls "$O/mod.d" | tr '\n' ' ')"
