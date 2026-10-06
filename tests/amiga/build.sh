#!/bin/sh
# build.sh -- t_amiga's inputs: the synthetic guest image and the user's
# A4000 Kickstart 3.2 ROM.
#
#   sh tests/amiga/build.sh outdir
#
# Out: outdir/root/tests/amiga/guest.bin,
# outdir/root/etc/amiga/kicka4000.rom.  AMIGAROM names the ROM (default:
# the local staging package's); proprietary, never in the repository.
# Without the ROM the guest tests still run and the ROM census skips.
# With the ROM, also startmig and the boot extension; with a prepared
# SYS: (AMIGASYS) and Picasso96 archive (AMIGAP96), outdir/sys.img: a
# ufs volume holding SYS: with the Picasso96 runtime and container.card.
# With IBrowse in SYS:, User-Startup also runs ibrowse/ibtest, which opens
# ibrowse/ibtest.html once t_amiga creates SYS:ibgo.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
OUT=$1
AMIGAROM=${AMIGAROM:-$AUX/images/work/amiga-stage/media/ROM/kicka4000.rom}
BIN=$AUX/toolchain/bin
rm -rf "$OUT"
if [ ! -f "$KDIR/guest/include/amigaio.h" ]; then
	echo "[skip] no Amiga guest"
	exit 0
fi
O=$OUT/obj
R=$OUT/root
mkdir -p "$O" "$R/tests/amiga"
"$BIN/m68k-elf-as" -m68040 --register-prefix-optional -o "$O/guest.o" "$T/amiga/guest.s"
"$BIN/m68k-elf-ld" --no-warn-rwx-segments -N -e start -Ttext=0x1000 -o "$O/guest.elf" "$O/guest.o"
"$BIN/m68k-elf-objcopy" -O binary "$O/guest.elf" "$R/tests/amiga/guest.bin"
if [ -f "$AMIGAROM" ]; then
	mkdir -p "$R/etc/amiga"
	cp "$AMIGAROM" "$R/etc/amiga/kicka4000.rom"
	sh "$KDIR/guest/amiga/build.sh" "$O/mig" > /dev/null
	sh "$KDIR/guest/amiga/dos/build.sh" "$O/dos" > /dev/null
	cp "$O/mig/startmig" "$R/tests/amiga/startmig"
	cp "$O/dos/container-boot.rom" "$R/etc/amiga/boot.rom"
fi
AMIGASYS=${AMIGASYS:-$AUX/images/work/amiga-stage/root/amiga/sys}
AMIGAP96=${AMIGAP96:-$AUX/images/work/amiga-stage/root/amiga/rtg/Picasso96.lha}
if [ -f "$AMIGAROM" ] && [ -f "$AMIGASYS/S/Startup-Sequence" ]; then
	sh "$KDIR/guest/amiga/rtg/build.sh" "$O/rtg" > /dev/null
	M=$O/sys.manifest
	(cd "$AMIGASYS" && find . -type d | sed 's#^\.##' | sort | sed '/^$/d; s#.*#d & 755 0 3#'
	 cd "$AMIGASYS" && find . -type f | sed 's#^\.##' | sort | sed "s#.*#f & 755 0 3 $AMIGASYS&#") > "$M"
	echo "f /LIBS/Picasso96/container.card 755 0 3 $O/rtg/container.card" >> "$M"
	# the current startup and the files binding Workbench to container.card
	G=$O/p96gen
	python3 "$AUX/tools/amiga/p96prefs.py" "$G" | sed 's#^Devs/#DEVS/#' > "$O/p96gen.list"
	python3 -c 'import sys; sys.path[0] = sys.argv[1]; from template import STARTUP; print(STARTUP, end="")' \
	    "$AUX/tools/amiga" > "$G/Startup-Sequence"
	echo S/Startup-Sequence >> "$O/p96gen.list"
	sh "$KDIR/guest/amiga/input/build.sh" "$O/input" > /dev/null
	echo C/container-input >> "$O/p96gen.list"
	awk 'NR == FNR { drop["/" tolower($0)]; next } !(tolower($2) in drop)' "$O/p96gen.list" "$M" > "$M.new"
	mv "$M.new" "$M"
	grep -q -i '^d /Prefs/Env-Archive/Picasso96 ' "$M" || echo "d /Prefs/Env-Archive/Picasso96 755 0 3" >> "$M"
	while read -r f; do
		case $f in
		S/*) echo "f /$f 755 0 3 $G/Startup-Sequence" ;;
		C/container-input) echo "f /$f 755 0 3 $O/input/container-input" ;;
		DEVS/*) echo "f /$f 755 0 3 $G/Devs/${f#DEVS/}" ;;
		*) echo "f /$f 755 0 3 $G/$f" ;;
		esac
	done < "$O/p96gen.list" >> "$M"
	if [ -f "$AMIGAP96" ]; then
		P=$O/p96/Picasso96Install
		7z x -y -o"$O/p96" "$AMIGAP96" > /dev/null
		for f in Libs/Picasso96/rtg.library Libs/Picasso96/emulation.library \
		    Libs/Picasso96/fastlayers.library Libs/Picasso96API.library; do
			echo "f /LIBS/${f#Libs/} 755 0 3 $P/$f" >> "$M"
		done
		echo "f /DEVS/Monitors/Container 755 0 3 $P/Devs/Monitors/Picasso96" >> "$M"
	fi
	# User-Startup also runs dragtest and, with IBrowse in SYS:, ibtest
	U=$(cd "$AMIGASYS/S" && ls | grep -i '^user-startup$' || :)
	{ [ -z "$U" ] || { cat "$AMIGASYS/S/$U"; echo; }; echo 'Run >NIL: Execute S:dragtest'
	  [ ! -f "$AMIGASYS/IBrowse/IBrowse" ] || echo 'Run >NIL: Execute S:ibtest'; } > "$O/User-Startup"
	[ -z "$U" ] || { sed "/ \/S\/$U /d" "$M" > "$M.new"; mv "$M.new" "$M"; }
	echo "f /S/${U:-User-Startup} 755 0 3 $O/User-Startup" >> "$M"
	echo "f /S/dragtest 755 0 3 $T/amiga/dragtest" >> "$M"
	if [ -f "$AMIGASYS/IBrowse/IBrowse" ]; then
		echo "f /S/ibtest 755 0 3 $T/amiga/ibrowse/ibtest" >> "$M"
		echo "f /ibtest.html 755 0 3 $T/amiga/ibrowse/ibtest.html" >> "$M"
	fi
	python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 32 -m /amiga/sys "$M" "$OUT/sys.img"
fi
echo "[ok] guest image, $(ls "$R/etc/amiga" 2>/dev/null | wc -l) ROM"
