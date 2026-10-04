#!/bin/sh
# amixld.sh -- link an AMIX program from a cc-style link line with the
# cross ld: crt files, shared libc, libm, libc's archive-only members.
#
#   amixld.sh -o out [cc flags] objects archives -Ldir -lname ...
#
# -lsocket, -lnsl and -lscreen resolve to the shared objects, as native
# clients link them.  libsocket.so needs syslog and seteuid, which only
# the static libc has; they are left for the run-time linker (lazy
# binding: they are never called by the server), any other diagnostic
# fails the link.
set -e
AUX=$(cd "$(dirname "$0")/../.." && pwd)
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
LD=$TC/bin/m68k-cbm-sysv4-ld
EXTRA=${AMIXLD_EXTRA:?libextra.a path}
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)

out=a.out
dirs="$SYS/usr/lib $SYS/usr/ccs/lib"
set -f
args=
while [ $# -gt 0 ]; do
	case $1 in
	-o) out=$2; shift ;;
	-L*) dirs="${1#-L} $dirs" ;;
	-l*)
		# by -l for a shared object: its name, not our path, goes in DT_NEEDED
		n=${1#-l} f=
		for d in $dirs; do
			[ -z "$f" ] && [ -f "$d/lib$n.so" ] && f="-L$d -l$n"
			[ -z "$f" ] && [ -f "$d/lib$n.a" ] && f=$d/lib$n.a
		done
		[ -n "$f" ] || { echo "amixld: no -l$n" >&2; exit 1; }
		args="$args $f" ;;
	-*) ;;
	*) args="$args $1" ;;
	esac
	shift
done

link() {
	"$LD" "$@" -o "$out" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" $args \
		"$SYS/usr/lib/libc.so.1" "$SYS/usr/ccs/lib/libm.a" "$EXTRA" "$LIBGCC" \
		"$SYS/usr/ccs/lib/crtn.o"
}
log=$out.ldlog
if link > "$log" 2>&1; then
	rm -f "$log"
	exit 0
fi
if grep -v -E "libsocket\.so: undefined reference to \`(syslog|seteuid)'$" "$log" | grep -q .; then
	cat "$log" >&2
	exit 1
fi
rm -f "$out"
link --noinhibit-exec > /dev/null 2>&1 || true
rm -f "$log"
[ -s "$out" ]
