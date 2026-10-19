#!/bin/sh
# mkmodpkgs.sh -- SVR4 datastreams of a build's loadable modules: one set
# per target and per-card packages.
#
#   sh images/envpkg/mkmodpkgs.sh indir outdir
#
# indir holds mod.d-TARGET/ (kernel/mods.sh's mod.d) for TARGET mac, amiga,
# atari030, atari060.  Out: outdir/NAME-VERSION.pkg, SHA256SUMS and
# .notes (relnotes.sh: the release text).
# VERSION defaults to version.sh's.  Per target (S = mac ami at30 at60):
#   cdfsS    CD filesystem        diskS    disk drivers (mac ncr96; amiga
#            amigaide ncr770; atari ataide scsi sd)
#   modS     modules no other package holds
# Amiga cards: amv3d (voodoo3 tdfx), amradeon (radeonds radeon), amzz9000,
#   amopci (opci pci), amfloppy, ambigram, amilib, amnet (ne xsurf)
# Atari: atnetS (aen dpn), atsndS (dmasnd auxsnd)
# Modules go to /usr/aux/lib/mod.d (mac), mod.amiga, mod.atari (030) or
# mod.atari060.  Missing modules are skipped.  The environment modules
# belong to the environment packages and dsuser to the base image: none
# goes in here.
set -e
D=$(cd "$(dirname "$0")" && pwd)
IN=$1 OUT=$2
case $OUT in ""|/) echo "usage: mkmodpkgs.sh indir outdir" >&2; exit 2 ;; esac
[ -d "$IN" ] || { echo "mkmodpkgs.sh: no $IN" >&2; exit 2; }
VERSION=${VERSION:-$(sh "$D/version.sh")}
OWNED="guestcore auxcore auxexec uinter tosguest amigaguest cpmguest tiplatform nextguest lisaguest dsuser"
rm -rf "$OUT"; mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
W=$OUT/.w; mkdir "$W"

pack() {	# target dir pkg "name" "needs" module...
	t=$1 dir=$2 p=$3 name=$4 needs=$5; shift 5
	r=$W/$p; rm -rf "$r"; mkdir -p "$r/usr/aux/lib/$dir"
	n=0
	for m; do
		echo "$m" >> "$W/owned"
		[ -f "$IN/mod.d-$t/$m" ] || continue
		cp "$IN/mod.d-$t/$m" "$r/usr/aux/lib/$dir/"; n=$((n+1))
	done
	[ $n -gt 0 ] || { echo "[skip] $p: no modules"; return 0; }
	chmod 644 "$r/usr/aux/lib/$dir"/*
	printf 'PKG=%s\nNAME=%s\nARCH=m68k\nVERSION=%s\nCATEGORY=system\nCLASSES=none\nVENDOR=Ash Nazag\n' \
		"$p" "$name" "$VERSION" > "$W/$p.info"
	printf 'i pkginfo=%s\n' "$W/$p.info" > "$W/$p.proto"
	if [ -n "$needs" ]; then
		for q in $needs; do printf 'P %s\t%s\n\t(m68k)%s\n' $q $q "$VERSION"; done > "$W/$p.dep"
		printf 'i depend=%s\n' "$W/$p.dep" >> "$W/$p.proto"
	fi
	python3 "$D/pkgmk.py" -r "$r" "$W/$p.proto" "$OUT/$p-$VERSION.pkg" PSTAMP=ashnazag-$p-$VERSION
	echo "$t|$p-$VERSION.pkg|$name|$*|$needs" >> "$OUT/.list"
	echo "[ok] $p: $n modules"
	rm -rf "$r"
}

rm -f "$OUT/.list"
for t in mac amiga atari030 atari060; do
	[ -d "$IN/mod.d-$t" ] || { echo "[skip] $t: no mod.d-$t"; continue; }
	case $t in
	mac)	s=mac dir=mod.d; disk="ncr96"; dd="SCSI (53C96)" ;;
	amiga)	s=ami dir=mod.amiga; disk="amigaide ncr770"; dd="Gayle IDE and A4000T/A4091 SCSI (53C710)" ;;
	atari030)	s=at30 dir=mod.atari; disk="ataide scsi sd"; dd="IDE and SCSI" ;;
	atari060)	s=at60 dir=mod.atari060; disk="ataide scsi sd"; dd="IDE and SCSI" ;;
	esac
	printf '%s\n' $OWNED > "$W/owned"
	pack $t $dir cdfs$s "CD-ROM file system (ISO 9660)" "" cdfs
	pack $t $dir disk$s "Disk drivers: $dd" "" $disk
	case $t in
	amiga)
		pack $t $dir amilib "AmigaOS library loader, for openpci.library" "" amilib
		pack $t $dir amopci "PCI bus behind a Mediator, Prometheus, G-REX or Firestorm" amilib opci pci
		pack $t $dir amv3d "Voodoo3/Banshee display and 3D (tdfx DRI)" amopci voodoo3 tdfx
		pack $t $dir amradeon "Radeon display and 3D" amopci radeonds radeon
		pack $t $dir amzz9000 "MNT ZZ9000 display" "" zz9000
		pack $t $dir amfloppy "Amiga floppy drive service" "" floppy
		pack $t $dir ambigram "Zorro III RAM above 1 GB as swap" "" bigram
		pack $t $dir amnet "Ethernet: NE2000 on PCI, X-Surf" "amopci amilib" ne xsurf ;;
	atari*)
		pack $t $dir atnet$s "Ethernet: NetUSBee, BlueSCSI DaynaPORT" disk$s aen dpn
		pack $t $dir atsnd$s "Sound: DMA sound, Mac environment sound" "" dmasnd auxsnd ;;
	esac
	o=$(ls "$IN/mod.d-$t" | grep -vxF -f "$W/owned" || :)
	pack $t $dir mod$s "Other modules" "" $o
done
sh "$D/relnotes.sh" "$OUT/.list" > "$OUT/.notes"
rm -rf "$W" "$OUT/.list"
cd "$OUT" && sha256sum ./*.pkg | sed 's| \./| |' > SHA256SUMS
