#!/bin/sh
# build.sh -- guest-process support: static shims and gates, modules.
#
#   sh kernel/guest/build.sh -k base outdir
#	static part for the relink: outdir/guest.o (shims, gate code,
#	gates generated from base's vector table) and outdir/gates.lst
#	for patch_vec.py
#   sh kernel/guest/build.sh -m kernel.elf outdir
#	modules against that kernel: outdir/mod.d/{guestcore,auxcore,auxexec,uinter}
#   sh kernel/guest/build.sh -t
#	host checks: interface headers compile, table sanity
#
# The kernel tree's dlm/ supplies <sys/moddefs.h> and mkmod.
set -e

G=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$G/.." && pwd)
AUX=$(cd "$K/.." && pwd)
DLM=$K/dlm
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
NICE="nice -n 19"
KCC="$NICE m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -Wall -I$DLM/include -I$G/include -I$G/mod/auxcore -I$K/mac/display -I$G/mod/tosguest"

# kernel objects must compile without warnings beyond the AMIX headers' own
kcc() {	# src obj
	$KCC -c "$1" -o "$2" 2> "$2.warn" || { cat "$2.warn"; exit 1; }
	if grep -v 'types.h:182\|strsubr.h:70\|In file included\|^ *from ' "$2.warn" | grep .; then
		echo "[FAIL] warnings in $1"; exit 1
	fi
}

case "$1" in
-k)
	BASE=$2; O=$3
	mkdir -p "$O"
	python3 "$G/mkgates.py" "$BASE" "$O/guest_vec.s" "$O/gates.lst"
	kcc "$G/guest_shim.c" "$O/guest_shim.o"
	$NICE m68k-cbm-sysv4-gcc -m68040 -c "$G/guest_gate.s" -o "$O/guest_gate.o"
	$NICE m68k-cbm-sysv4-gcc -m68040 -c "$O/guest_vec.s" -o "$O/guest_vec.o"
	m68k-cbm-sysv4-ld -r -o "$O/guest.o" "$O/guest_shim.o" "$O/guest_gate.o" \
		"$O/guest_vec.o"
	echo "[OK] $O/guest.o"
	;;
-m)
	KERNEL=$2; O=$3
	mkdir -p "$O/tools" "$O/src"
	HCC="$NICE ${HOSTCC:-cc} -std=gnu89 -O -w"
	$HCC -DDLM_TOOL -I"$DLM" -I"$DLM/include" -o "$O/tools/mkksym" \
		"$DLM/tools/mkksym.c" "$DLM/dlm_sym.c"
	$HCC -o "$O/tools/modfix" "$DLM/tools/modfix.c"
	"$O/tools/mkksym" -x "$KERNEL" > "$O/exports"
	rm -rf "$O/mod.d"
	for m in guestcore auxcore auxexec uinter tosguest amigaguest; do
		d=$O/src/$m
		rm -rf "$d"; mkdir -p "$d"
		objs=
		for c in "$G/mod/$m"/*.c; do
			kcc "$c" "$d/$(basename "$c" .c).o"
			objs="$objs $d/$(basename "$c" .c).o"
		done
		m68k-cbm-sysv4-ld -r -o "$d/Driver.o" $objs
		cp "$G/mod/$m/Master" "$d/"
		DLM_TOOLS=$O/tools sh "$DLM/tools/mkmod" -e "$O/exports" -o "$O" "$d"
	done
	;;
-t)
	O=$G/build/test
	mkdir -p "$O"
	# interface headers, alone and together, as the kernel compiles them
	for h in guest.h guest_rpage.h guest_rom.h; do
		printf '#include "kinc.h"\n#include "%s"\n' "$h" > "$O/h.c"
		kcc "$O/h.c" "$O/h.o"
	done
	printf '#include "kinc.h"\n#include "guest_rpage.h"\n#include "guest_rom.h"\nstruct guest_ctr c; struct guest_rpage rp; struct guest_romdesc rd; struct guest_profile pf;\n' > "$O/h.c"
	kcc "$O/h.c" "$O/h.o"
	python3 "$G/test/chktab.py" "$G/mod/auxcore/auxcalls.c" "$G/mod/auxcore/auxconv.c"
	${HOSTCC:-cc} -std=gnu89 -w -o "$O/t_auxsock" "$G/test/t_auxsock.c" \
		"$G/mod/auxcore/auxsockx.c"
	"$O/t_auxsock"
	;;
*)
	echo "usage: build.sh -k base outdir | -m kernel.elf outdir | -t" >&2
	exit 2
	;;
esac
