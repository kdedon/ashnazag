#!/bin/sh
# build.sh -- cross-build software Quake (quake-amix) for our display service.
#
#   sh gl/quake/build.sh
#
# Needs ref/quake-amix (tools/setup.sh --gl) and toolchain/amix-gcc295
# (toolchain/build-gcc295.sh).  CC295=no builds with gcc 2.7.2.3 instead.
# ARCH (default 68040) is the -m target.  Out: $QW/quake (default
# images/work/quake).
set -e

D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
QSRC=${QSRC:-$AUX/ref/quake-amix}
QW=${QW:-$AUX/images/work/quake}
ARCH=${ARCH:-68040}
T=m68k-cbm-sysv4
OLD=$AUX/toolchain/amix
SYS=$OLD/$T/sysroot
N="nice -n 19"
if [ "${CC295:-yes}" = no ]; then
	TC=$OLD
	OPT="-O"
else
	TC=$AUX/toolchain/amix-gcc295
	OPT="-O2 -fomit-frame-pointer"
fi
CC=$TC/bin/$T-gcc
LIBGCC=$(ls "$TC"/lib/gcc-lib/$T/*/libgcc.a | tail -1)
TMPDIR=$QW/tmp
export TMPDIR

[ -x "$CC" ] || { echo "$CC missing: sh toolchain/build-gcc295.sh" >&2; exit 1; }
[ -f "$QSRC/src/host.c" ] || { echo "$QSRC missing: sh tools/setup.sh --gl" >&2; exit 1; }

CORE="chase cl_demo cl_input cl_main cl_parse cl_tent cmd common console crc cvar
	d_edge d_fill d_init d_modech d_part d_polyse d_scan d_sky d_sprite d_surf
	d_vars d_zpoint draw host host_cmd keys mathlib menu model net_dgrm net_loop
	net_main net_vcr nonintel pr_cmds pr_edict pr_exec r_aclip r_alias r_bsp
	r_draw r_edge r_efrag r_light r_main r_misc r_part r_sky r_sprite r_surf
	r_vars sbar screen sv_main sv_move sv_phys sv_user view wad world zone
	sys_amix net_amix snd_null cd_null"

S=$QW/src
O=$QW/obj-$ARCH-$(basename "$TC")
mkdir -p "$S" "$O" "$TMPDIR"
cp -p "$QSRC"/src/*.[chs] "$QSRC"/src/*.q[12] "$D"/src/*.c "$AUX/kernel/mac/display/dsio.h" "$S/"
for p in "$D"/patches/*.diff; do
	[ -f "$p" ] || continue
	patch -s -p1 -d "$QW" < "$p"
done

# extra members of libc's archive part (gettimeofday, the fp control
# functions libm wants), without the shared object itself
if [ ! -f "$QW/libextra.a" ]; then
	rm -rf "$QW/extra"; mkdir -p "$QW/extra"
	(cd "$QW/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
	 ar rc ../libextra.a $(ar t "$SYS/usr/ccs/lib/libc.so" | grep -v '^libc.so.1$'))
fi

objs=
for f in $CORE vid_fb in_fb; do
	if [ ! -f "$O/$f.o" ] || [ "$S/$f.c" -nt "$O/$f.o" ]; then
		$N "$CC" -m$ARCH $OPT -DAMIX -I"$S" -c "$S/$f.c" -o "$O/$f.o"
	fi
	objs="$objs $O/$f.o"
done
$N "$OLD/bin/$T-ld" -o "$QW/quake" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
	$objs "$SYS/usr/lib/libc.so.1" "$SYS/usr/ccs/lib/libm.a" "$QW/libextra.a" \
	"$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
echo "$QW/quake"
