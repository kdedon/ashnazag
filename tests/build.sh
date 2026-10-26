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
# the compiler GL code is built with (t_dlopen's shared objects)
sh "$AUX/gl/mkgcc.sh" "$O/gcc"
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

glcc() {	# src obj [flags]
	src=$1; obj=$2; shift 2
	nice -n 19 "$O/gcc" -O2 -Wall -Wno-implicit -Wno-return-type "$@" -I"$T/src" -c "$src" -o "$obj"
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
NHI="-I$NH -I$KDIR/guest -I$KDIR/guest/voyage200 -I$KDIR/mac/sound -I$T/nativecall"
for c in nativehost nativehost_platform nativehost_ui; do
	cc "$NH/$c.c" "$O/$c.o" $XA -I"$NH" -I"$KDIR/mac/display"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-as" -o "$O/nativecall.o" "$NH/nativecall.s"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-as" -o "$O/ncprobe.o" "$T/nativecall/probe.s"
# t_kstat's library and top, as the top package builds them
sh "$AUX/images/envpkg/top/build.sh" "$O/top" > "$O/top.log" 2>&1 || { tail "$O/top.log"; exit 1; }
cp "$O/top/top" "$BIN/top"
for s in ${ONLY:-"$T"/src/t_*.c "$T/src/runall.c"}; do
	n=$(basename "$s" .c)
	[ -n "$ONLY" ] || case $n in t_stable|t_smsqe|t_swp) continue ;; esac
	if [ $n = t_dlopen ]; then
		glcc "$T/src/dlso_a.c" "$O/dlso_a.o" -fPIC
		glcc "$T/src/dlso_b.c" "$O/dlso_b.o" -fPIC
		glcc "$T/src/dlso_c.c" "$O/dlso_c.o"
		nice -n 19 "$LD" -shared -soname /tests/libdla.so -o "$BIN/libdla.so" "$O/dlso_a.o"
		nice -n 19 "$LD" -shared -o "$BIN/libdlb.so" "$O/dlso_b.o" "$O/dlso_c.o"
		cc "$s" "$O/$n.o"
		link "$BIN/$n" -E "$O/$n.o" "$O/t.o" "$BIN/libdla.so" "$SYS/usr/ccs/lib/libdl.so"
		echo "[ok] $n"
		continue
	fi
	if [ $n = t_gl ]; then
		if [ -f "$GLW/lib/libOSMesa.a" ]; then
			glcc "$s" "$O/$n.o" -m68020-40 -D_XOPEN_SOURCE -I"$GLW/include"
			link "$BIN/$n" "$O/$n.o" "$O/t.o" "$GLW/lib/libOSMesa.a" "$GLW/lib/libfastmem.a"
		else
			cc "$s" "$O/$n.o" -DNOGL
			link "$BIN/$n" "$O/$n.o" "$O/t.o"
		fi
		echo "[ok] $n"
		continue
	fi
	cc "$s" "$O/$n.o" $XA -I"$KDIR/dlm/include" -I"$KDIR/mac/display" -I"$KDIR/mac/sound" \
		-I"$KDIR/guest/mod/tosguest" -I"$KDIR/guest/include" -I"$KDIR/guest/amiga" -I"$KDIR/net" \
		-I"$KDIR/amiga/radeon" -I"$KDIR/kstat" $NHI
	extra=
	case $n in t_dlm|t_display|t_guestdlm|t_envram|t_aux|t_mac|t_mac6|t_mac76|t_mac81|t_ot81|t_coff81|t_coff81d|t_coff76|t_coff76c|t_tos|t_xaaes|t_mint|t_cpm|t_smsqe|t_amiga|t_sound|t_stable|t_scsi|t_swp) extra=$MODO ;; esac
	case $n in
	t_nativehost) extra="$O/nativehost.o $MODO" ;;
	t_nhplat) extra=$O/nativehost_platform.o ;;
	t_nativecall) extra="$O/nativecall.o $O/ncprobe.o $O/nativehost.o $MODO" ;;
	t_v200|t_next|t_lisa) extra=$MODO ;;
	# a NeXT guest: linked above the guest's space
	t_gmmu) HOST=$(sed -n 's/^#define NX_HOST *\(0x[0-9a-f]*\)UL.*/\1/p' "$KDIR/guest/next/profile.h")
		"$LD" --verbose | sed -e '1,/^=====/d' -e '/^=====/,$d' -e "s/0x80000000/$HOST/" > "$O/next.ld"
		extra="-T $O/next.ld $MODO" ;;
	t_radeon) cc "$KDIR/amiga/radeon/rdsinit.c" "$O/rdsinit.o"; extra=$O/rdsinit.o ;;
	t_kstat) extra="$O/top/libkstat.a $MODO" ;;
	esac
	link "$BIN/$n" "$O/$n.o" "$O/t.o" $extra
	echo "[ok] $n"
done
# t_natenv's launcher and payload
cc "$KDIR/guest/nativeenv/startnative.c" "$O/startnative.o" $XA -I"$KDIR/guest/include"
link "$BIN/startnative" "$O/startnative.o"
cc "$T/src/natpayload.c" "$O/natpayload.o" $XA
link "$BIN/natpayload" "$O/natpayload.o"
# startlisa: Lisa OS from the user's volume, run natively
cc "$KDIR/guest/lisaboot/startlisa.c" "$O/startlisa.o" $XA -I"$NH" -I"$KDIR/mac/display" -I"$KDIR/guest/include"
link "$BIN/startlisa" "$O/startlisa.o" "$O/nativehost.o"
# t_env runs startmac's lock helper
cc "$AUX/images/macenv/envlock.c" "$O/envlock.o" $XA -w
link "$BIN/envlock" "$O/envlock.o"
# the TOS environment's counters
cc "$T/src/tosstat.c" "$O/tosstat.o" $XA -I"$KDIR/guest/mod/tosguest"
link "$BIN/tosstat" "$O/tosstat.o"
cc "$AUX/images/macenv/macscrn.c" "$O/macscrn.o" $XA -I"$KDIR/mac/display"
link "$BIN/macscrn" "$O/macscrn.o"
if [ -n "$MODO" ]; then
	cc "$KDIR/dlm/modadmin.c" "$O/modadmin.o" $XA -w -I"$KDIR/dlm/include"
	link "$BIN/modadmin" "$O/modadmin.o" $MODO
	cc "$KDIR/dlm/kvar.c" "$O/kvar.o" $XA -w -I"$KDIR/dlm/include"
	link "$BIN/kvar" "$O/kvar.o" $MODO
	cp "$KDIR/mac/diskroot/etc/scsibench" "$BIN/scsibench"
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
	# t_dhcp's DHCP client, as its package builds it
	D=$AUX/images/envpkg/sdhcp
	cc "$D/sdhcp.c" "$O/sdhcp.o"
	cc "$D/eprintf.c" "$O/eprintf.o"
	link "$B/netbin/sdhcp" "$O/sdhcp.o" "$O/eprintf.o" -L"$SYS/usr/lib" -lsocket -lnsl
	cp "$D/dhcpinfo" "$B/netbin/dhcpinfo"
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
