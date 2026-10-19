#!/bin/sh
# mkenvpkgs.sh -- the environment packages, built from source: SVR4
# datastreams for pkgadd or apkg on Mac, Atari and Amiga hosts.
#
#   sh images/envpkg/mkenvpkgs.sh outdir
#
#   envcore   the guest module, the module path (S04envcore), the admin
#             file envpkg for unattended pkgadd and pkgrm
#   macenv    startmac, the A/UX modules (S05aux)
#   macnet    the Open Transport bridge (S06macnet)
#   tosenv    starttos, EmuTOS, fVDI, XaAES, TeraDesk, /tos (S05tos)
#   amigaenv  startamiga, the Workbench drivers (S05amiga)
#   cpmenv    startcpm (with DRI's release only: it builds against it)
#   smsqenv   startsmsq and SMSQ/E (with its binaries zip)
#   win16env  startwin
#   lisaenv   startlisa, the native Lisa host (S05lisa)
#   v200env   startvoyage200, the native Voyage 200 host (S05v200)
#
# Out: outdir/pkgs/env/PKG-VERSION.pkg, outdir/catalog in apkg's format,
# outdir/guest/home/guest (guest's TOS, SMSQ and WIN16 folders, for disk
# images) and outdir/dri/cpm (DRI's CP/M files, likewise).  The modules are
# KERNEL's (default kernel/build/unix-mac.elf); builds that differ for
# ATARIKERNEL, ATARI060KERNEL or AMIGAKERNEL (default kernel/build's
# unix-atari030.elf, unix-atari060.elf, unix-amiga.elf, if present) go in
# /usr/aux/lib/mod.atari, mod.atari060 or mod.amiga, by the kernel's
# target.  VERSION defaults to version.sh's.
# ROMs, OS media and other proprietary files never go in.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
OUT=$1
case $OUT in ""|/) echo "usage: mkenvpkgs.sh outdir" >&2; exit 2 ;; esac
KERNEL=${KERNEL:-$AUX/kernel/build/unix-mac.elf}
ATARIKERNEL=${ATARIKERNEL:-$AUX/kernel/build/unix-atari030.elf}
ATARI060KERNEL=${ATARI060KERNEL:-$AUX/kernel/build/unix-atari060.elf}
AMIGAKERNEL=${AMIGAKERNEL:-$AUX/kernel/build/unix-amiga.elf}
VERSION=${VERSION:-$(sh "$D/version.sh")}
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
G=$AUX/kernel/guest
M=$AUX/images/macenv
CC="nice -n 19 $TC/bin/m68k-cbm-sysv4-gcc -O -w -D__STDC__=0"
rm -rf "$OUT"
mkdir -p "$OUT/pkgs/env" "$OUT/guest" "$OUT/w/lm"
OUT=$(cd "$OUT" && pwd)
W=$OUT/w

link68() {	# out objects...
	o=$1; shift
	"$TC/bin/m68k-cbm-sysv4-ld" -o "$o" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
		"$@" "$SYS/usr/lib/libc.so.1" "$SYS/usr/ccs/lib/crtn.o"
}
for s in "$AUX"/kernel/dlm/libmod/*.s; do
	"$TC/bin/m68k-cbm-sysv4-as" -o "$W/lm/$(basename "$s" .s).o" "$s"
done

# the modules, built for each host's kernel
mods() {	# kernel host
	sh "$G/build.sh" -m "$1" "$W/g.$2" > "$W/g.$2.log" 2>&1 || { tail "$W/g.$2.log"; exit 1; }
	sh "$AUX/kernel/otbridge/build.sh" -k "$1" "$W/o.$2" > "$W/o.$2.log" 2>&1 ||
		{ tail "$W/o.$2.log"; exit 1; }
	mkdir -p "$W/mod.$2"
	cp "$W/g.$2"/mod.d/* "$W/o.$2/mod.d/otbridge" "$W/mod.$2/"
	rm -rf "$W/g.$2" "$W/o.$2"
}
mods "$KERNEL" mac
H=
for k in "$ATARIKERNEL" "$ATARI060KERNEL" "$AMIGAKERNEL"; do
	[ -f "$k" ] || continue
	case `sh "$AUX/kernel/mods.sh" -t "$k"` in
	amiga) h=amiga ;;
	atari030) h=atari ;;
	atari060) h=atari060 ;;
	*) continue ;;
	esac
	case " $H " in *" $h "*) continue ;; esac
	mods "$k" $h; H="$H $h"
done
mod() {		# package module...
	p=$1; shift
	for m; do
		mkdir -p "$W/$p/usr/aux/lib/mod.d"
		cp "$W/mod.mac/$m" "$W/$p/usr/aux/lib/mod.d/"
		for h in $H; do
			cmp -s "$W/mod.mac/$m" "$W/mod.$h/$m" && continue
			mkdir -p "$W/$p/usr/aux/lib/mod.$h"
			cp "$W/mod.$h/$m" "$W/$p/usr/aux/lib/mod.$h/"
		done
	done
}

# envcore
mod envcore guestcore
mkdir -p "$W/envcore/etc/rc2.d" "$W/envcore/var/sadm/install/admin"
cp "$D/envcore/moddirs" "$D/envcore/modfree" "$W/envcore/usr/aux/lib/"
cp "$D/envcore/admin" "$W/envcore/var/sadm/install/admin/envpkg"
cp "$D/envcore/S04envcore" "$W/envcore/etc/rc2.d/"

# macenv
mod macenv auxcore auxexec uinter
R=$W/macenv
mkdir -p "$R/usr/bin" "$R/etc/rc2.d" "$R/etc/aux"
$CC -I"$AUX/kernel/dlm/include" -c "$AUX/kernel/mac/diskroot/pkg/auxreg.c" -o "$W/auxreg.o"
link68 "$R/usr/aux/lib/auxreg" "$W/auxreg.o" "$W"/lm/*.o
$CC -c "$M/macdiag.c" -o "$W/macdiag.o"
link68 "$R/usr/bin/macdiag" "$W/macdiag.o" "$W"/lm/*.o
$CC -I"$AUX/kernel/mac/display" -c "$M/macscrn.c" -o "$W/macscrn.o"
link68 "$R/usr/bin/macscrn" "$W/macscrn.o" "$W"/lm/*.o
for c in envlock mac6; do
	$CC -c "$M/$c.c" -o "$W/$c.o"
done
link68 "$R/usr/bin/envlock" "$W/envlock.o"
link68 "$R/usr/aux/lib/mac6" "$W/mac6.o"
cp "$M/startmac" "$R/usr/bin/startmac"
cp "$M/S05aux" "$R/etc/rc2.d/"
chmod 755 "$R"/usr/bin/*

# macnet
mod macnet otbridge
R=$W/macnet
mkdir -p "$R/usr/sbin" "$R/etc/rc2.d"
$CC -I"$AUX/kernel/dlm/include" -c "$M/network/otreg.c" -o "$W/otreg.o"
link68 "$R/usr/sbin/otreg" "$W/otreg.o" "$W"/lm/*.o
cp "$D/macnet/S06macnet" "$R/etc/rc2.d/"

# tosenv
sh "$AUX/images/tosenv/mktos.sh" "$W/tos" > "$W/tos.log" 2>&1 || { tail "$W/tos.log"; exit 1; }
mv "$W/tos/root" "$W/tosenv"
mod tosenv tosguest
mkdir -p "$W/tosenv/etc/rc2.d"
cp "$D/tosenv/S05tos" "$W/tosenv/etc/rc2.d/"
cp -R "$W/tos/guest/." "$OUT/guest/"

# amigaenv
R=$W/amigaenv
A=$G/amiga
mod amigaenv amigaguest
mkdir -p "$R/usr/bin" "$R/usr/sbin" "$R/etc/rc2.d" "$R/etc/amiga" "$R/amiga/media" "$R/amiga/rtg" "$W/am"
sh "$A/build.sh" "$W/am" > "$W/am.log" 2>&1 || { tail "$W/am.log"; exit 1; }
cp "$W/am/startamiga" "$R/usr/bin/"
$CC -I"$AUX/kernel/dlm/include" -I"$G/include" -c "$AUX/images/amigaenv/amigareg.c" -o "$W/amigareg.o"
link68 "$R/usr/sbin/amigareg" "$W/amigareg.o" "$W"/lm/*.o
sh "$A/rtg/build.sh" "$W/rtg" > "$W/rtg.log" 2>&1 || { tail "$W/rtg.log"; exit 1; }
cp "$W/rtg/container.card" "$A/rtg/README.md" "$A/rtg/NOTICE" "$R/amiga/rtg/"
for d in dos input session bsdsock ahi; do
	sh "$A/$d/build.sh" "$R/amiga/guest/$d" > "$W/am-$d.log" 2>&1 || { tail "$W/am-$d.log"; exit 1; }
done
cp "$A/dos/README.md" "$A/dos/CONTAINER" "$R/amiga/guest/dos/"
cp "$A/input/README.md" "$R/amiga/guest/input/"
cp "$R/amiga/guest/dos/container-boot.rom" "$R/etc/amiga/"
cp "$D/amigaenv/S05amiga" "$R/etc/rc2.d/"
chmod 644 "$R/etc/amiga/container-boot.rom" "$R/amiga/guest/dos/container-boot.rom"

# cpmenv: DRI's files stay out
sh "$AUX/images/cpmenv/mkcpm.sh" "$W/cpm" > "$W/cpm.log" 2>&1 || { tail "$W/cpm.log"; exit 1; }
if [ -f "$W/cpm/root/usr/bin/startcpm" ]; then
	mv "$W/cpm/root" "$W/cpmenv"
	mod cpmenv cpmguest
	mv "$W/cpm/dri" "$OUT/dri"
fi

# smsqenv
sh "$AUX/images/smsqenv/mksmsq.sh" "$W/smsq" > "$W/smsq.log" 2>&1 || { tail "$W/smsq.log"; exit 1; }
if [ -f "$W/smsq/root/usr/bin/startsmsq" ]; then
	mv "$W/smsq/root" "$W/smsqenv"
	cp -R "$W/smsq/guest/." "$OUT/guest/"
fi

# win16env
sh "$AUX/images/winenv/mkwin.sh" "$W/win" > "$W/win.log" 2>&1 || { tail "$W/win.log"; exit 1; }
if [ -f "$W/win/root/usr/bin/startwin" ]; then
	mv "$W/win/root" "$W/win16env"
	cp -R "$W/win/guest/." "$OUT/guest/"
fi

# lisaenv, v200env: launcher (one program, named per environment) and native host
$CC -I"$G/include" -c "$G/nativeenv/startnative.c" -o "$W/startnative.o"
for e in "lisa lisaenv lisaguest startlisa lisaboot" "voyage200 v200env tiplatform startvoyage200 voyage200"; do
	set -- $e
	d=$1 p=$2 m=$3 l=$4 s=$5
	R=$W/$p
	mod $p $m
	mkdir -p "$R/usr/bin" "$R/usr/lib/$d" "$R/etc/rc2.d"
	link68 "$R/usr/bin/$l" "$W/startnative.o"
	sh "$G/$s/build.sh" "$W/$d" > "$W/$d.log" 2>&1 || { tail "$W/$d.log"; exit 1; }
	cp "$W/$d/native" "$R/usr/lib/$d/"
	cp "$D/$p"/S05* "$R/etc/rc2.d/"
done

# the datastreams and their catalog
echo '#apkg-catalog 1' > "$OUT/catalog"
for p in envcore macenv macnet tosenv amigaenv cpmenv smsqenv win16env lisaenv v200env; do
	if [ ! -d "$W/$p" ]; then
		echo "[skip] $p: inputs missing (see $W/*.log)"
		continue
	fi
	f=env/$p-$VERSION.pkg
	# depend: every P line wants this build's version
	rm -rf "$W/meta"; cp -R "$D/$p" "$W/meta"
	[ ! -f "$D/$p/depend" ] ||
		awk -v v="$VERSION" '{ print } /^P/ { print "\t(m68k)" v }' "$D/$p/depend" > "$W/meta/depend"
	python3 "$D/pkgmk.py" -r "$W/$p" "$W/meta/prototype" "$OUT/pkgs/$f" \
		VERSION=$VERSION PSTAMP=ashnazag-$p-$VERSION
	deps=`sed -n 's/^P[	 ]*\([a-z0-9]*\).*/\1/p' "$D/$p/depend" 2> /dev/null | paste -sd, -`
	name=`sed -n 's/^NAME=//p' "$D/$p/pkginfo"`
	echo "$p|$VERSION|$p|$f|`md5sum < "$OUT/pkgs/$f" | cut -d' ' -f1`|`wc -c < "$OUT/pkgs/$f"`|${deps:-none}|system|$name" >> "$OUT/catalog"
	echo "[ok] $p: `wc -c < "$OUT/pkgs/$f"` bytes"
done
rm -rf "$W"
