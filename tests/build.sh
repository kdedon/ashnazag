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
for s in ${ONLY:-"$T"/src/t_*.c "$T/src/runall.c"}; do
	n=$(basename "$s" .c)
	cc "$s" "$O/$n.o" $XA -I"$KDIR/dlm/include" -I"$KDIR/mac/display" \
		-I"$KDIR/guest/mod/tosguest" -I"$KDIR/guest/include"
	extra=
	case $n in t_dlm|t_aux|t_mac|t_mac76|t_mac81|t_tos|t_amiga) extra=$MODO ;; esac
	link "$BIN/$n" "$O/$n.o" "$O/t.o" $extra
	echo "[ok] $n"
done
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
