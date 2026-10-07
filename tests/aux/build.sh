#!/bin/sh
# build.sh -- t_aux's inputs: the guest modules, built against the
# kernel under test, A/UX programs and files from a local A/UX root,
# and abi, a freestanding A/UX program built here.
#
#   sh tests/aux/build.sh kernel.elf outdir
#
# Out: outdir/mod.d/{guestcore,auxcore,auxexec,uinter}, outdir/bin/<program>,
# outdir/root/<path> (shared libraries, terminfo, termcap; for t_mac the
# Mac environment: startmac, libmac1_s, Patch.067C, the System file in
# /mac/sys/Sys7 with the Finder, the ROM image at /etc/aux/rom, fidd at
# /etc/aux/fidd; with the A/UX 2.0.1 CD image, CD201, its System 6
# environment in /a201).  With the Mac OS 7.6.1 CD image, CD761, or the
# Mac OS 8.1 one, CD81, outdir/macsys.img: a ufs volume holding their
# System Folders with SimpleText (S761, S761u with a user's Finder, S81).
# AUXROOT names the A/UX root, AUXROM the Mac ROM image (default: the
# Quadra 700 ROM beside the repository); both proprietary, never in the
# repository.
# A kernel without guest support gets nothing, and t_aux skips.
set -e

T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
# A/UX root: $AUXROOT, else the path in tests/aux/auxroot, else /aux
[ -n "$AUXROOT" ] || [ ! -f "$T/aux/auxroot" ] || AUXROOT=$(cat "$T/aux/auxroot")
AUXROOT=${AUXROOT:-/aux}
KERNEL=$1
OUT=$2
PATH="$AUX/toolchain/bin:$PATH"
export PATH

rm -rf "$OUT"
if ! m68k-elf-nm "$KERNEL" | grep -q ' T guest_trap$'; then
	echo "[skip] $KERNEL has no guest support"
	exit 0
fi
sh "$KDIR/guest/build.sh" -m "$KERNEL" "$OUT"
mkdir -p "$OUT/bin"
for p in sh ls cp mv rm rmdir awk more sleep; do
	[ -f "$AUXROOT/bin/$p" ] && cp "$AUXROOT/bin/$p" "$OUT/bin/$p"
done
[ -f "$AUXROOT/usr/bin/vi" ] && cp "$AUXROOT/usr/bin/vi" "$OUT/bin/vi"
for f in shlib/libc1_s usr/lib/terminfo/v/vt100 etc/termcap; do
	if [ -f "$AUXROOT/$f" ]; then
		mkdir -p "$OUT/root/$(dirname "$f")"
		cp "$AUXROOT/$f" "$OUT/root/$f"
	fi
done

# the Mac environment, when the kernel has the uinter module
AUXROM=${AUXROM:-$AUX/420DBFF3 - Quadra 700&900 & PB140&170.ROM}
SYSF="$AUXROOT/mac/sys/System Folder/System"
if [ -f "$OUT/mod.d/uinter" ] && [ -f "$AUXROOT/mac/bin/startmac" ] && [ -f "$SYSF" ] &&
    [ -f "$AUXROM" ]; then
	mkdir -p "$OUT/root/mac/bin" "$OUT/root/mac/lib/Patches" "$OUT/root/mac/sys/Sys7" \
		"$OUT/root/shlib" "$OUT/root/etc/aux"
	cp "$AUXROOT/mac/bin/startmac" "$OUT/root/mac/bin/"
	cp "$AUXROOT/shlib/libmac1_s" "$OUT/root/shlib/"
	cp "$AUXROOT/mac/lib/Patches/Patch.067C" "$OUT/root/mac/lib/Patches/"
	# Shut Down's dialog resources; the test root's names have no spaces
	mkdir -p "$OUT/root/mac/lib/Resources"
	if [ -f "$AUXROOT/mac/lib/Resources/%AUX Resources" ]; then
		cp "$AUXROOT/mac/lib/Resources/%AUX Resources" "$OUT/root/mac/lib/Resources/%AUXResources"
	fi
	cp "$SYSF" "$OUT/root/mac/sys/Sys7/System"
	cp "$AUXROOT/mac/lib/SystemFiles/shared/Finder" "$OUT/root/mac/sys/Sys7/Finder"
	cp "$AUXROM" "$OUT/root/etc/aux/rom"
	# SysBeep's sound on a Quadra; t_sound's INIT
	cp "$AUXROOT/mac/lib/Resources/sampledBeep" "$OUT/root/mac/lib/Resources/"
	sh "$T/aux/sndtest/build.sh" "$OUT/root/mac/lib/SndTest"
	# the File ID daemon, which the Mac side needs to create folders
	cp "$AUXROOT/etc/fidd" "$OUT/root/etc/aux/fidd"
	# Mac OS 7.6.1 and 8.1 from their CDs, when present, on a ufs volume
	# (long names): S761, S761u with a user's Finder, S81
	V=$OUT/macsys
	CD761=${CD761:-$AUX/media/Mac OS 7.6.1.iso}
	if [ -f "$CD761" ]; then
		sh "$AUX/images/macenv/mksys76.sh" "$CD761" "$V/S761"
		cp -rp "$V/S761" "$V/S761u"
		python3 "$AUX/images/macenv/userfinder.py" "$V/S761u/%Finder"
	fi
	# mtcp, t_mactcp's Mac application, a startup item there, and the resolver
	LX=$AUX/toolchain/linux/bin/m68k-linux-gnu
	if [ -x "$LX-gcc" ]; then
		mkdir -p "$OUT/root/mac/sys/MTcp" "$OUT/obj"
		nice -n 19 "$LX-gcc" -m68020 -mpcrel -fcall-used-d2 -O -ffreestanding -fno-builtin -nostdlib \
			-Wl,-Ttext=0 -Wl,--build-id=none -o "$OUT/obj/mtcp.elf" \
			"$T/net/mtcp/mtcp0.s" "$T/net/mtcp/mtcp.c"
		"$LX-objcopy" -O binary -j .text -j .rodata "$OUT/obj/mtcp.elf" "$OUT/obj/mtcp.bin"
		python3 "$T/net/mtcp/mkapp.py" "$OUT/obj/mtcp.bin" "$OUT/root/mac/sys/MTcp/mtcp"
		cp "$AUXROOT/mac/sys/System Folder/MacTCP DNR" "$OUT/root/mac/sys/MTcp/MacTCPDNR"
	fi
	# A/UX 2.0.1's System 6 environment, a root of its own (chroot), from
	# its CD, when present; the test root's names have no spaces, t_mac6
	# links the System Folder's under their own
	CD201=${CD201:-$AUX/media/AUX_2.0.1_CD_Image.iso}
	if [ -f "$CD201" ]; then
		R6=$OUT/root/a201
		sh "$AUX/images/macenv/mksys6.sh" "$CD201" "$R6"
		mv "$R6/mac/sys/System Folder" "$R6/mac/sys/Sys6"
		for n in "%AUX Resources" "DA Handler" "Key Layout" "Scrapbook File"; do
			mv "$R6/mac/sys/Sys6/$n" "$R6/mac/sys/Sys6/$(echo "$n" | tr -d ' ')"
		done
		mkdir -p "$R6/dev" "$R6/etc/aux"
		# the IIci's ROM, the one System 6.0.7 knows, else the other
		AUXROM6=${AUXROM6:-$AUX/368CADFE - Mac IIci.ROM}
		[ -f "$AUXROM6" ] || AUXROM6=$AUXROM
		cp "$AUXROM6" "$R6/etc/aux/rom"
		mkdir -p "$R6/mac/lib/Resources"
		cp "$AUXROOT/mac/lib/Resources/sampledBeep" "$R6/mac/lib/Resources/"
	fi
	CD81=${CD81:-$AUX/media/MacOS8_1.iso}
	[ ! -f "$CD81" ] || sh "$AUX/images/macenv/mksys81.sh" "$CD81" "$V/S81"
	if [ -d "$V" ]; then
		(cd "$V" && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc -R 0:3 --quiet) > "$OUT/macsys.cpio"
		mb=$(du -sm "$V" | cut -f1)
		echo "a macsys.cpio" > "$OUT/macsys.manifest"
		python3 "$KDIR/mac/diskroot/mkufs.py" -s $(((mb * 5 / 4 + 8) / 4 * 4)) -r "$OUT" \
			-m /macsys "$OUT/macsys.manifest" "$OUT/macsys.img"
		rm -rf "$V" "$OUT/macsys.cpio" "$OUT/macsys.manifest"
	fi
fi

TC=$AUX/toolchain/amix
O=$OUT/obj
mkdir -p "$O"
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -m68020 -fno-builtin \
	-c "$T/aux/abi.c" -o "$O/abi.o"
"$TC/bin/m68k-cbm-sysv4-as" -o "$O/abi0.o" "$T/aux/abi0.s"
"$TC/bin/m68k-cbm-sysv4-ld" -T "$T/aux/abi.ld" -o "$O/abi.elf" "$O/abi0.o" "$O/abi.o" \
	"$LIBGCC"
python3 "$T/aux/elf2aux.py" "$O/abi.elf" "$OUT/bin/abi"
# macabi: a Mac task without startmac (text at 0x100000a8, clear of Mac RAM)
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -m68020 -fno-builtin \
	-c "$T/aux/macabi.c" -o "$O/macabi.o"
"$TC/bin/m68k-cbm-sysv4-as" -o "$O/macabi0.o" "$T/aux/macabi0.s"
sed 's/0x10a8/0x100000a8/; s/0x400000/0x10400000/' "$T/aux/abi.ld" > "$O/macabi.ld"
"$TC/bin/m68k-cbm-sysv4-ld" -T "$O/macabi.ld" -o "$O/macabi.elf" "$O/macabi0.o" \
	"$O/macabi.o" "$LIBGCC"
python3 "$T/aux/elf2aux.py" "$O/macabi.elf" "$OUT/bin/macabi"
# macjoin: a second process in a Mac session, with macabi's start-up
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -m68020 -fno-builtin \
	-c "$T/aux/macjoin.c" -o "$O/macjoin.o"
"$TC/bin/m68k-cbm-sysv4-ld" -T "$O/macabi.ld" -o "$O/macjoin.elf" "$O/macabi0.o" \
	"$O/macjoin.o" "$LIBGCC"
python3 "$T/aux/elf2aux.py" "$O/macjoin.elf" "$OUT/bin/macjoin"
echo "[ok] $(ls "$OUT/mod.d" | wc -l) modules, $(ls "$OUT/bin" | wc -l) A/UX programs"
