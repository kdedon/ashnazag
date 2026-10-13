#!/bin/sh
# build.sh -- compile the test programs into build/bin.
#
#   sh tests/build.sh
#
# Linked against the shared libc like the rest of the root.  Calls that live
# only in libc's nonshared archive part (select, vfork, gettimeofday, ...) are
# taken from that archive after the shared object.
set -e

T=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
CC=$TC/bin/m68k-cbm-sysv4-gcc
LD=$TC/bin/m68k-cbm-sysv4-ld
STRIP=$TC/bin/m68k-cbm-sysv4-strip
B=$T/build
O=$B/obj
BIN=$B/bin
CFLAGS="-O -Wall -Wno-implicit -Wno-return-type"
# __STDC__=0 opens the headers' full SVR4 namespace, like the native cc -Xa;
# t.c keeps __STDC__=1 for <stdarg.h>
XA=-D__STDC__=0
export TMPDIR="${TMPDIR:-$B/tmp}"

mkdir -p "$O" "$BIN" "$TMPDIR"
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
# t_cc295's second compiler; without it t_cc295 skips
CC295=$AUX/toolchain/amix-gcc295/bin/m68k-cbm-sysv4-gcc
LIBGCC295=$(ls "$AUX"/toolchain/amix-gcc295/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a 2>/dev/null | tail -1)
[ -x "$CC295" ] || CC295=$CC
# t_gl's library (gl/build.sh); without it t_gl skips
GLW=${GLW:-$AUX/images/work/gl}

# nonshared libc members, without the libc.so.1 member itself
if [ ! -f "$O/libextra.a" ]; then
	rm -rf "$O/extra"; mkdir -p "$O/extra"
	(cd "$O/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
	 ar rc ../libextra.a $(ar t "$SYS/usr/ccs/lib/libc.so" | grep -v '^libc.so.1$'))
fi

cc() {	# src obj [flags]
	src=$1; obj=$2; shift 2
	nice -n 19 "$CC" $CFLAGS "$@" -I"$T/src" -c "$src" -o "$obj"
}

cc295() {	# src obj [flags]
	src=$1; obj=$2; shift 2
	nice -n 19 "$CC295" -O2 -Wall -Wno-implicit -Wno-return-type "$@" -I"$T/src" -c "$src" -o "$obj"
}

link() {	# out obj...
	out=$1; shift
	nice -n 19 "$LD" -o "$out" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
		"$@" "$SYS/usr/lib/libc.so.1" "$SYS/usr/ccs/lib/libm.a" "$O/libextra.a" \
		"$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
}

cc "$T/src/t.c" "$O/t.o"
# loadable-module system call stubs, for t_dlm and modadmin
MODO=
for s in "$KDIR"/dlm/libmod/*.s; do
	[ -f "$s" ] || continue
	n=lm_$(basename "$s" .s)
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-as" -o "$O/$n.o" "$s"
	MODO="$MODO $O/$n.o"
done
# the native-environment host library and adapters, for t_native* and t_v200
NH=$KDIR/guest/nativehost
NHI="-I$NH -I$KDIR/guest/lisa -I$KDIR/guest/voyage200 -I$KDIR/mac/sound -I$T/nativecall"
for c in nativehost nativehost_platform nativehost_ui; do
	cc "$NH/$c.c" "$O/$c.o" $XA -I"$NH" -I"$KDIR/mac/display"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-as" -o "$O/nativecall.o" "$NH/nativecall.s"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-as" -o "$O/ncprobe.o" "$T/nativecall/probe.s"
for s in ${ONLY:-"$T"/src/t_*.c "$T/src/runall.c"}; do
	n=$(basename "$s" .c)
	[ -n "$ONLY" ] || [ $n != t_stable ] || continue
	if [ $n = t_cc295 ]; then
		cc "$s" "$O/$n.o"
		cc295 "$T/src/cc295abi.c" "$O/cc295abi.o"
		link "$BIN/$n" "$O/$n.o" "$O/cc295abi.o" "$O/t.o" $LIBGCC295
		cc295 "$T/src/t_arith.c" "$O/cc295_arith.o" $XA
		cc295 "$T/src/t.c" "$O/cc295_t.o"
		link "$BIN/cc295_arith" "$O/cc295_arith.o" "$O/cc295_t.o" $LIBGCC295
		echo "[ok] $n"
		continue
	fi
	if [ $n = t_gl ]; then
		if [ -f "$GLW/lib/libOSMesa.a" ] && [ "$CC295" != "$CC" ]; then
			cc295 "$s" "$O/$n.o" -m68040 -D_XOPEN_SOURCE -I"$GLW/include"
			link "$BIN/$n" "$O/$n.o" "$O/t.o" "$GLW/lib/libOSMesa.a" $LIBGCC295
		else
			cc "$s" "$O/$n.o" -DNOGL
			link "$BIN/$n" "$O/$n.o" "$O/t.o"
		fi
		echo "[ok] $n"
		continue
	fi
	cc "$s" "$O/$n.o" $XA -I"$KDIR/dlm/include" -I"$KDIR/mac/display" -I"$KDIR/mac/sound" \
		-I"$KDIR/guest/mod/tosguest" -I"$KDIR/guest/include" -I"$KDIR/guest/amiga" -I"$KDIR/net" \
		-I"$KDIR/amiga/radeon" $NHI
	extra=
	case $n in t_dlm|t_display|t_guestdlm|t_envram|t_aux|t_mac|t_mac6|t_mac76|t_mac81|t_ot81|t_coff81|t_coff81d|t_coff76|t_coff76c|t_tos|t_xaaes|t_mint|t_cpm|t_smsqe|t_amiga|t_sound|t_stable) extra=$MODO ;; esac
	case $n in
	t_nativehost) extra="$O/nativehost.o $MODO" ;;
	t_nhplat) extra=$O/nativehost_platform.o ;;
	t_nativecall) extra="$O/nativecall.o $O/ncprobe.o $O/nativehost.o $MODO" ;;
	t_v200) extra=$MODO ;;
	t_lisa) extra=$MODO ;;
	t_radeon) cc "$KDIR/amiga/radeon/rdsinit.c" "$O/rdsinit.o"; extra=$O/rdsinit.o ;;
	esac
	link "$BIN/$n" "$O/$n.o" "$O/t.o" $extra
	echo "[ok] $n"
done
# t_natenv's launcher and payload
cc "$KDIR/guest/nativeenv/startnative.c" "$O/startnative.o" $XA -I"$KDIR/guest/include"
link "$BIN/startnative" "$O/startnative.o"
cc "$T/src/natpayload.c" "$O/natpayload.o" $XA
link "$BIN/natpayload" "$O/natpayload.o"
# t_lisa's adapter checks: lh_<name>, from tests/lisa/<name>.c
LI="$XA -I$KDIR/guest/lisa -I$NH -I$KDIR/mac/display -I$KDIR/mac/sound"
L=$KDIR/guest/lisa
cc "$L/lisa_hw.c" "$O/lisa_hw.o" $LI
cc "$L/lisa_host.c" "$O/lisa_host.o" $LI
cc "$L/lisa_host.c" "$O/lisa_host_ti.o" $LI -Dioctl=test_ioctl
for c in hardware render; do
	cc "$T/lisa/$c.c" "$O/lh_$c.o" $LI
done
# its variadic test_ioctl needs <stdarg.h>, so __STDC__=1
cc "$T/lisa/publish.c" "$O/lh_publish.o" ${LI#$XA}
link "$BIN/lh_hardware" "$O/lh_hardware.o" "$O/lisa_hw.o"
link "$BIN/lh_render" "$O/lh_render.o" "$O/lisa_host.o" "$O/nativehost_ui.o"
link "$BIN/lh_publish" "$O/lh_publish.o" "$O/lisa_host_ti.o" "$O/nativehost_ui.o"
# t_env runs startmac's lock helper
cc "$AUX/images/macenv/envlock.c" "$O/envlock.o" $XA -w
link "$BIN/envlock" "$O/envlock.o"
if [ -n "$MODO" ]; then
	cc "$KDIR/dlm/modadmin.c" "$O/modadmin.o" $XA -w -I"$KDIR/dlm/include"
	link "$BIN/modadmin" "$O/modadmin.o" $MODO
	echo "[ok] modadmin"
fi

# otb/t_*: Open Transport bridge, with the module calls and otwire.h
if [ -z "$ONLY" ] && [ -f "$KDIR/otbridge/otwire.h" ]; then
	for s in "$T"/otb/t_*.c; do
		n=$(basename "$s" .c)
		cc "$s" "$O/$n.o" $XA -I"$KDIR/dlm/include" -I"$KDIR/otbridge"
		link "$BIN/$n" "$O/$n.o" "$O/t.o" $MODO
		echo "[ok] $n"
	done
fi

# net/t_*: also libsocket and libnsl; into netbin, for the net root only
if [ -z "$ONLY" ]; then
	mkdir -p "$B/netbin"
	for s in "$T"/net/t_*.c; do
		n=$(basename "$s" .c)
		cc "$s" "$O/$n.o" $XA -I"$KDIR/dlm/include"
		link "$B/netbin/$n" "$O/$n.o" "$O/t.o" $MODO -L"$SYS/usr/lib" -lsocket -lnsl
		echo "[ok] $n"
	done
fi

# xh_<kind><target>: exec targets whose stripped file size mod 4096 is
# <target> (d: small, L: over 64 KB of .data); d0 is the plain helper.
xh() {	# kind size name
	python3 "$T/src/genpad.py" "$1" "$2" "$O/xhpad.h"
	cc "$T/src/xh.c" "$O/xh.o" -I"$O" $XA
	link "$BIN/$3" "$O/xh.o"
	nice -n 19 "$STRIP" "$BIN/$3"
	wc -c < "$BIN/$3"
}
[ -n "$NOXH" ] && exit 0
rm -f "$BIN"/xh_*
xh data 0 xh_d0 >/dev/null
for spec in d:16 d:1000 d:2047 d:2048 d:2049 d:3000 d:4095 d:4096 D:1 D:2049; do
	k=${spec%%:*}; tgt=${spec#*:}
	kind=data
	pad=4096
	[ "$k" = D ] && pad=70000	# spans many pages
	i=0
	while :; do
		sz=$(xh $kind $pad xh_tmp)
		[ -n "$XHDEBUG" ] && echo "  $spec pad $pad size $sz" >&2
		m=$((sz % 4096)); [ $m -eq 0 ] && m=4096
		d=$((tgt - m))
		[ $d -gt 2048 ] && d=$((d - 4096))
		[ $d -le -2048 ] && d=$((d + 4096))
		if [ $d -ge 0 ] && [ $d -le 7 ]; then
			# section table is 8-aligned: pad the file tail to the exact size
			[ $d -gt 0 ] && head -c $d /dev/zero >> "$BIN/xh_tmp"
			sz=$((sz + d))
			break
		fi
		i=$((i + 1))
		[ $i -ge 6 ] && { echo "[FAIL] xh $spec: size $sz" >&2; exit 1; }
		[ $d -lt 0 ] && d=$((d - 8))
		pad=$((pad + d))
	done
	name=xh_$(echo $k | tr D L)$tgt
	mv "$BIN/xh_tmp" "$BIN/$name"
	echo "[ok] $name size $sz (pad $pad)"
done
