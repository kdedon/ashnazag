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
# /etc/aux/fidd; with the Mac OS 7.6.1 CD image, CD761, its System
# Folder in /mac/sys/S761).
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
	cp "$SYSF" "$OUT/root/mac/sys/Sys7/System"
	cp "$AUXROOT/mac/lib/SystemFiles/shared/Finder" "$OUT/root/mac/sys/Sys7/Finder"
	cp "$AUXROM" "$OUT/root/etc/aux/rom"
	# the File ID daemon, which the Mac side needs to create folders
	cp "$AUXROOT/etc/fidd" "$OUT/root/etc/aux/fidd"
	# Mac OS 7.6.1 from its CD, when present
	CD761=${CD761:-$AUX/media/Mac OS 7.6.1.iso}
	[ ! -f "$CD761" ] ||
		sh "$AUX/images/macenv/mksys76.sh" "$CD761" "$OUT/root/mac/sys/S761"
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
echo "[ok] $(ls "$OUT/mod.d" | wc -l) modules, $(ls "$OUT/bin" | wc -l) A/UX programs"
