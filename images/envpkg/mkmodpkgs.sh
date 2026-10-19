#!/bin/sh
# mkmodpkgs.sh -- SVR4 datastreams of a build's loadable modules: one set
# per target and per-card packages.
#
#   sh images/envpkg/mkmodpkgs.sh indir outdir
#
# indir holds mod.d-TARGET/ (kernel/mods.sh's mod.d) for TARGET mac, amiga,
# atari030, atari060.  Out: outdir/NAME-VERSION.pkg and SHA256SUMS.
# VERSION defaults to unstable.  Per target (S = mac ami at30 at60):
#   modS     all modules          envmS    environment and display modules
#   cdfsS    CD filesystem        diskS    disk drivers (mac ncr96; amiga
#            amigaide ncr770; atari ataide scsi sd)
# Amiga cards: amv3d (voodoo3 tdfx), amradeon (radeonds radeon), amzz9000,
#   amopci (opci pci), amfloppy, ambigram, amilib, amnet (ne xsurf)
# Atari: atnetS (aen dpn), atsndS (dmasnd auxsnd)
# Modules go to /usr/aux/lib/mod.d (mac), mod.amiga, mod.atari (030) or
# mod.atari060.  Missing modules are skipped.
set -e
D=$(cd "$(dirname "$0")" && pwd)
IN=$1 OUT=$2
case $OUT in ""|/) echo "usage: mkmodpkgs.sh indir outdir" >&2; exit 2 ;; esac
[ -d "$IN" ] || { echo "mkmodpkgs.sh: no $IN" >&2; exit 2; }
VERSION=${VERSION:-unstable}
ENVM="guestcore auxcore auxexec uinter tosguest amigaguest cpmguest tiplatform nextguest lisaguest dsuser"
rm -rf "$OUT"; mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
W=$OUT/.w; mkdir "$W"

pack() {	# target dir pkg name module...
	t=$1 dir=$2 p=$3 name=$4; shift 4
	r=$W/$p; rm -rf "$r"; mkdir -p "$r/usr/aux/lib/$dir"
	n=0
	for m; do
		[ -f "$IN/mod.d-$t/$m" ] || continue
		cp "$IN/mod.d-$t/$m" "$r/usr/aux/lib/$dir/"; n=$((n+1))
	done
	[ $n -gt 0 ] || { echo "[skip] $p: no modules"; return 0; }
	chmod 644 "$r/usr/aux/lib/$dir"/*
	printf 'PKG=%s\nNAME=%s\nARCH=m68k\nVERSION=%s\nCATEGORY=system\nCLASSES=none\nVENDOR=Ash Nazag\n' \
		"$p" "$name" "$VERSION" > "$W/$p.info"
	printf 'i pkginfo=%s\n' "$W/$p.info" > "$W/$p.proto"
	python3 "$D/pkgmk.py" -r "$r" "$W/$p.proto" "$OUT/$p-$VERSION.pkg" PSTAMP=ashnazag-$p-$VERSION
	echo "[ok] $p: $n modules"
	rm -rf "$r"
}

for t in mac amiga atari030 atari060; do
	[ -d "$IN/mod.d-$t" ] || { echo "[skip] $t: no mod.d-$t"; continue; }
	case $t in
	mac)	s=mac dir=mod.d; disk="ncr96" ;;
	amiga)	s=ami dir=mod.amiga; disk="amigaide ncr770" ;;
	atari030)	s=at30 dir=mod.atari; disk="ataide scsi sd" ;;
	atari060)	s=at60 dir=mod.atari060; disk="ataide scsi sd" ;;
	esac
	pack $t $dir mod$s "Modules, $t" $(ls "$IN/mod.d-$t")
	pack $t $dir envm$s "Environment and display modules, $t" $ENVM
	pack $t $dir cdfs$s "CD filesystem, $t" cdfs
	pack $t $dir disk$s "Disk drivers, $t" $disk
	case $t in
	amiga)
		pack $t $dir amv3d "Voodoo3 display and tdfx 3D" voodoo3 tdfx
		pack $t $dir amradeon "Radeon display and 3D" radeonds radeon
		pack $t $dir amzz9000 "ZZ9000 graphics" zz9000
		pack $t $dir amopci "OpenPCI and PCI bus" opci pci
		pack $t $dir amfloppy "Floppy service" floppy
		pack $t $dir ambigram "Big RAM" bigram
		pack $t $dir amilib "AmigaOS library host" amilib
		pack $t $dir amnet "Ethernet: NE2000 and X-Surf" ne xsurf ;;
	atari*)
		pack $t $dir atnet$s "Network drivers, $t" aen dpn
		pack $t $dir atsnd$s "Sound drivers, $t" dmasnd auxsnd ;;
	esac
done
rm -rf "$W"
cd "$OUT" && sha256sum ./*.pkg | sed 's| \./| |' > SHA256SUMS
