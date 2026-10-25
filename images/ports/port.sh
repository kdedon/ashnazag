# port.sh -- sourced by images/ports/<name>/build.sh after D (its directory):
#
#   PORT=name OUT=outdir; . "$AUX/images/ports/port.sh"
#
# Sets CC (cc.sh), PCFLAGS (the SVR4 declarations; libport's in every
# file), HOST (configure options), LIBPORT (libacompat, the passwd
# parsers), W (work dir), ROOT (the installed tree) and
# PATH (the cross tools).  Helpers:
#
#   src NAME          unpack images/vendor.manifest's NAME into W, print
#                     its top directory
#   log NAME CMD...   run CMD, output in W/NAME.log, its tail on failure
#   portpkg           gate every ELF file in ROOT (FPU: the ones that may
#                     have FPU instructions), then write
#                     outdir/pkgs/PKG-VERSION.pkg (pkginfo's VERSION) and
#                     its catalog line, outdir/PKG.cat (apkg's format)
PD=$AUX/images/ports
case $OUT in ""|/) echo "usage: build.sh outdir" >&2; exit 2 ;; esac
OUT=$(mkdir -p "$OUT/pkgs" && cd "$OUT" && pwd)
W=$OUT/w/$PORT
ROOT=$W/root
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
LB=$AUX/toolchain/linux/bin
CC="sh $PD/cc.sh"
PCFLAGS="-O -Wimplicit -D__STDC__=0 -I$PD/compat/include -Wp,-include,$PD/compat/include/port.h"
HOST="--host=m68k-cbm-sysv4 --build=`uname -m`-pc-linux-gnu --prefix=/opt/amix"
NICE="nice -n 19"
LIBPORT=$OUT/w/libport.a
rm -rf "$W"; mkdir -p "$W/bin" "$W/tmp" "$ROOT/opt/amix/bin"
# the cross ar crashes; the host one writes the same archives
ln -s "$LB/m68k-linux-gnu-ar" "$W/bin/m68k-cbm-sysv4-ar"
ln -s "$LB/m68k-linux-gnu-ranlib" "$W/bin/m68k-cbm-sysv4-ranlib"
PATH=$W/bin:$TC/bin:$LB:$PATH
TMPDIR=$W/tmp
export PATH TMPDIR LIBPORT

if [ ! -f "$LIBPORT" ]; then
	rm -rf "$OUT/w/lp"; mkdir -p "$OUT/w/lp"
	for s in "$PD"/compat/*.c; do
		$NICE $CC -O -w -I"$PD/compat/include" -c "$s" -o "$OUT/w/lp/$(basename "$s" .c).o"
	done
	# libm calls the FPU mode setters only libc.a has
	(cd "$OUT/w/lp" && m68k-linux-gnu-ar x "$SYS/usr/ccs/lib/libc.a" \
		fpgetmask.o fpgetrnd.o fpgetsticky.o fpsetrnd.o fpsetsticky.o fpsetmask.o)
	m68k-linux-gnu-ar rcs "$LIBPORT.tmp" "$OUT"/w/lp/*.o
	mv "$LIBPORT.tmp" "$LIBPORT"
fi

src() {
	t=$(sh "$AUX/images/fetch.sh" "$1")
	case $t in *.bz2) z=bzip2 ;; *) z=gzip ;; esac
	$z -dc "$t" | tar xf - -C "$W"
	$z -dc "$t" | tar tf - | sed -n '1s,/.*,,p'
}
log() {
	n=$1; shift
	"$@" > "$W/$n.log" 2>&1 || { tail -20 "$W/$n.log"; exit 1; }
}

portpkg() {
	for l in "$SYS/usr/lib/libc.so.1" "$SYS/usr/lib/libnsl.so" "$SYS/usr/lib/libsocket.so"; do
		m68k-linux-gnu-readelf -sW "$l" | awk '$7 != "UND" && $5 != "LOCAL" { print $8 }'
	done | sort -u > "$W/rt.syms"
	bad=0
	for b in $(find "$ROOT" -type f); do
		case $(head -c 4 "$b" | od -An -c | tr -d ' ') in 177ELF) ;; *) continue ;; esac
		read -r x i f <<EOI
$(sh "$PD/scan060.sh" "$b")
EOI
		n=${b#$ROOT/}
		echo "  $n: 060-missing $i, fpu $f"
		[ "$i" = 0 ] || bad=1
		# FPU instructions only in the binaries FPU names, where float
		# formats alone reach them
		case " $FPU " in *" ${b##*/} "*) ;; *) [ "$f" = 0 ] || bad=1 ;; esac
		# libc's own passwd lookup crashes; the parsers in libport must win
		! m68k-linux-gnu-nm "$b" 2>/dev/null | grep -q ' U getpw\(nam\|uid\|ent\)$' ||
			{ echo "  $n: getpw* from libc"; bad=1; }
		u=$(m68k-linux-gnu-readelf -sW "$b" | awk '$7 == "UND" && $8 != "" { print $8 }' |
			sort -u | comm -23 - "$W/rt.syms")
		[ -z "$u" ] || { echo "  $n: no library has" $u; bad=1; }
		"$AUX/toolchain/bin/m68k-elf-strip" "$b"
	done
	# s5 roots keep 14 bytes of a name
	long=$(cd "$ROOT" && find . -name '???????????????*')
	[ -z "$long" ] || { echo "  names over 14 bytes: $long"; bad=1; }
	[ $bad = 0 ] || { echo "[FAIL] $PORT"; exit 1; }
	n=$(sed -n 's/^PKG=//p' "$D/pkginfo")
	v=$(sed -n 's/^VERSION=//p' "$D/pkginfo")
	f=$n-$v.pkg
	python3 "$AUX/images/envpkg/pkgmk.py" -r "$ROOT" "$D/prototype" "$OUT/pkgs/$f" PSTAMP=ashnazag-$n-$v
	deps=$(sed -n 's/^P[	 ]*\([a-z0-9]*\).*/\1/p' "$D/depend" 2> /dev/null | paste -sd, -)
	echo "$n|$v|$n|$f|$(md5sum < "$OUT/pkgs/$f" | cut -d' ' -f1)|$(wc -c < "$OUT/pkgs/$f")|${deps:-none}|$(sed -n 's/^CATEGORY=//p' "$D/pkginfo")|$(sed -n 's/^NAME=//p' "$D/pkginfo")" > "$OUT/$n.cat"
	echo "[ok] $n $v: $(wc -c < "$OUT/pkgs/$f") bytes"
}
