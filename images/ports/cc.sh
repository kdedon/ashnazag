#!/bin/sh
# cc.sh -- the AMIX gcc for packages that run on every 68k host.
#
# Compiles go to the gcc wrapper at -m68020; it expands the 64-bit
# multiply the 68060 lacks.
# Links call ld directly: libsocket.so and libnsl.so by bare name (a
# path would be recorded as their DT_NEEDED), other libraries static,
# libc.so.1, then the libc.so archive for the calls libc.so.1 does not
# export (syslog, siglongjmp ...).  Every link gets LIBPORT; SYSINC
# replaces the system include directory.
P=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$P/../.." && pwd)
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
GCC=$TC/bin/m68k-cbm-sysv4-gcc
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
set -f
# gcc 2.7 rejects the newer warning options
n=$#
for a; do
	case $a in -Wall|-Wimplicit*|-W[lpa],*) ;; -W*) a= ;; esac
	[ -n "$a" ] && set -- "$@" "$a"
done
shift $n
mode=link o=
for a; do
	case $a in
	-c) mode=c ;;
	-E|-S|-M|-MM|-v|--version|-V*) mode=pass ;;
	esac
done
# the system headers after the caller's -I directories (a port's
# userdefs.h, say, must win), not before them as the gcc wrapper puts them
export AMIX_SYSROOT=$P/nonexistent
INC=-I${SYSINC:-$SYS/usr/include}
[ $mode = pass ] && exec "$GCC" "$@" $INC

# -include, which the gcc wrapper drops: a source that includes both
if [ $mode = c ]; then
	prev= inc= src=
	for a; do
		case $prev in -o) o=$a ;; -include) inc="$inc $a" ;; esac
		case $a in *.c) src=$a ;; esac
		prev=$a
	done
	if [ -n "$inc" ] && [ -n "$src" ]; then
		[ -n "$o" ] || o=$(basename "$src" .c).o
		t=${TMPDIR:-/tmp}/inc$$.c
		{ for f in $inc; do echo "#include \"$(cd "$(dirname "$f")" && pwd)/$(basename "$f")\""; done
		  echo "#include \"$(cd "$(dirname "$src")" && pwd)/$(basename "$src")\""; } > "$t"
		args= prev=
		for a; do
			case $prev in -o|-include) prev=; continue ;; esac
			prev=$a
			case $a in -o|-include) continue ;; "$src") a=$t ;; esac
			args="$args $a"
		done
		sh "$0" $args -o "$o"
		r=$?; rm -f "$t"; exit $r
	fi
	exec "$GCC" -m68020 "$@" $INC
fi

# link: compile sources first, then one ld command
out=a.out objs= libs= dirs="-L$SYS/usr/ccs/lib" tmp= prev= flags= cf=
for a; do
	case $a in -[IDU]*|-O*|-f*|-m*|-g*|-w|-ansi|-traditional) cf="$cf $a" ;; -W[!l]*) cf="$cf $a" ;; esac
done
for a; do
	case $prev in
	-o) out=$a; prev=; continue ;;
	-L) dirs="$dirs -L$a"; prev=; continue ;;
	-l) a=-l$a ;;
	esac
	prev=
	case $a in
	-o|-L|-l) prev=$a ;;
	*.c)	t=${TMPDIR:-/tmp}/cc$$.$(basename "$a" .c).o
		sh "$0" -c $cf "$a" -o "$t" || exit
		tmp="$tmp $t"; objs="$objs $t" ;;
	*.o|*.a) objs="$objs $a" ;;
	-L*)	dirs="$dirs $a" ;;
	-lc|-lgcc|-ldl|-lintl) ;;
	-lsocket|-lnsl) libs="$libs -L$SYS/usr/lib $a" ;;
	-l*)	n=${a#-l} f=
		for d in $(echo "$dirs" | sed 's/-L//g') $SYS/usr/lib; do
			[ -f "$d/lib$n.a" ] && { f=$d/lib$n.a; break; }
		done
		[ -n "$f" ] || { echo "cc.sh: no lib$n.a" >&2; exit 1; }
		libs="$libs $f" ;;
	-Wl,*)	flags="$flags $(echo "${a#-Wl,}" | tr , ' ')" ;;
	-s)	flags="$flags -s" ;;
	esac
done
"$TC/bin/m68k-cbm-sysv4-ld" -o "$out" $flags "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
	$objs $LIBPORT $libs $LIBPORT "$SYS/usr/lib/libc.so.1" "$SYS/usr/ccs/lib/libc.so" \
	"$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
r=$?
[ -z "$tmp" ] || rm -f $tmp
exit $r
