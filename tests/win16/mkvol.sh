#!/bin/sh
# mkvol.sh -- t_win16's inputs: startwin for AMIX and a ufs volume
# (SCSI disk 3) with the test programs, their expected results and, when
# found, the user's Windows 3.11 setup disks and Wabi 2.2 package.
#
#   sh tests/win16/mkvol.sh outdir
#
# Out: outdir/startwin, outdir/win16.img.  WIN311 names the Windows 3.11
# disks: a .7z holding DISK*.IMG or a directory of them (default: the
# retail 3.5" set in media/).  WABI names Wabi's pkgs/SUNWwabi (default:
# media/Wabi's x86 one).  Both proprietary: copied into the volume only.
# x86diff goes on too, with a corpus from the 80386 suite when SST names
# a checkout of it (default ref/sst-80386).
# Without the AMIX toolchain nothing is built and t_win16 skips.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
OUT=$1
WIN311=${WIN311:-$AUX/media/Microsoft Windows 3.11 (Retail Full) (3.5).7z}
WABI=${WABI:-$AUX/media/Wabi/x86/pkgs/SUNWwabi}
case $OUT in ""|/) echo "usage: mkvol.sh outdir" >&2; exit 2 ;; esac
rm -rf "$OUT"
[ -x "$AUX/toolchain/amix/bin/m68k-cbm-sysv4-gcc" ] || { echo "[skip] no AMIX toolchain"; exit 0; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
sh "$T/win16/build.sh" "$OUT/b"
mv "$OUT/b/startwin" "$OUT/b/x86diff" "$OUT/"
rm -rf "$OUT/b"
S=$OUT/src
M=$OUT/manifest
mkdir -p "$S/progs" "$S/expect" "$S/disks"
{
	echo "d /progs 755 0 3"
	echo "d /expect 755 0 3"
	echo "d /home 777 0 3"
	echo "d /t 777 0 3"
} > "$M"
for p in "$T"/build/win16/progs/*.exe; do
	[ -f "$p" ] || continue
	cp "$p" "$S/progs/"
	echo "f /progs/$(basename "$p") 644 0 3 progs/$(basename "$p")" >> "$M"
done
cp "$OUT/x86diff" "$S/"
echo "f /x86diff 755 0 3 x86diff" >> "$M"
SST=${SST:-$AUX/ref/sst-80386}
if [ -d "$SST/v1_ex_real_mode" ]; then
	python3 "$T/win16/mkcorpus.py" "$S/x86corpus" $(ls "$SST"/v1_ex_real_mode/*.MOO.gz | grep -v '/F4\.MOO')
	echo "f /x86corpus 644 0 3 x86corpus" >> "$M"
fi
for e in "$T"/win16/expect/*.txt; do
	cp "$e" "$S/expect/"
	echo "f /expect/$(basename "$e") 644 0 3 expect/$(basename "$e")" >> "$M"
done
case $WIN311 in
*.7z)	[ ! -f "$WIN311" ] || 7z e -y -o"$S/disks" "$WIN311" '*/DISK*.IMG' > /dev/null ;;
*)	[ ! -d "$WIN311" ] || cp "$WIN311"/DISK*.IMG "$S/disks/" ;;
esac
if [ -n "$(ls "$S/disks" 2>/dev/null)" ] && [ -f "$WABI/pkgmap" ]; then
	echo "d /disks 755 0 3" >> "$M"
	for d in "$S"/disks/*; do
		echo "f /disks/$(basename "$d") 644 0 3 disks/$(basename "$d")" >> "$M"
	done
	cp -R "$WABI" "$S/wabi"
	echo "d /wabi 755 0 3" >> "$M"
	(cd "$S" && find wabi -mindepth 1 -type d | sort) | while read -r d; do
		echo "d /$d 755 0 3"
	done >> "$M"
	(cd "$S" && find wabi -type f | sort) | while read -r f; do
		echo "f /$f 644 0 3 $f"
	done >> "$M"
	echo "[ok] Windows 3.11 and Wabi 2.2 on the volume"
else
	echo "[skip] no Windows 3.11 disks or Wabi package: programs only"
fi
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 96 -t 723000000 -r "$S" -m /win "$M" "$OUT/win16.img"
rm -rf "$S"
echo "[ok] $OUT/win16.img"
