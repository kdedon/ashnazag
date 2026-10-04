#!/bin/sh
# build.sh -- t_tos's inputs: starttos, the machine-layer cartridge, the
# system C: folder with C:\AUTO\UTEST.PRG (checks the host drives),
# EmuTOS and the user's TOS ROM image.
#
#   sh tests/tos/build.sh outdir
#
# Out: outdir/root/tos/bin/starttos, outdir/root/tos/{sys,fvdi,teradesk}/,
# outdir/root/etc/tos/{emutos.img,rom,tosml.img}.
# EMUTOS names the EmuTOS 512 KB release zip (default: ref/emutos-release),
# EMUTOSLANG its image (us).  TOSROM names the user's ROM: an image, or a
# zip holding one (default: the TOS 3.06 zip beside the repository);
# proprietary, never in the repository.  Without either nothing is built,
# and t_tos skips.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
G=$KDIR/guest
OUT=$1
TOSROM=${TOSROM:-$AUX/tos306us-american-24-09-1991.zip}
EMUTOS=${EMUTOS:-$AUX/ref/emutos-release/emutos-512k-1.4.zip}
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
BIN=$AUX/toolchain/bin
rm -rf "$OUT"
if { [ ! -f "$TOSROM" ] && [ ! -f "$EMUTOS" ]; } || [ ! -f "$G/tos/starttos.c" ]; then
	echo "[skip] no EmuTOS, no TOS ROM or no starttos"
	exit 0
fi
O=$OUT/obj
R=$OUT/root
mkdir -p "$O" "$R/tos/bin" "$R/etc/tos"
[ -f "$EMUTOS" ] && unzip -p "$EMUTOS" "*/etos512${EMUTOSLANG:-us}.img" > "$R/etc/tos/emutos.img"
[ -s "$R/etc/tos/emutos.img" ] || rm -f "$R/etc/tos/emutos.img"
case $TOSROM in
*.zip)	unzip -p "$TOSROM" '*.img' > "$R/etc/tos/rom" ;;
*)	[ -f "$TOSROM" ] && cp "$TOSROM" "$R/etc/tos/rom" ;;
esac
[ -s "$R/etc/tos/rom" ] || rm -f "$R/etc/tos/rom"
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
for c in starttos tosdisp; do
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -Wno-implicit -D__STDC__=0 \
		-I"$G/mod/tosguest" -I"$KDIR/mac/display" -c "$G/tos/$c.c" -o "$O/$c.o"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$R/tos/bin/starttos" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$O/starttos.o" "$O/tosdisp.o" "$SYS/usr/lib/libc.so.1" \
	"$T/build/obj/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
XCC="nice -n 19 $TC/bin/m68k-cbm-sysv4-gcc -O -m68020 -Wall -Wno-implicit -fno-builtin"
$XCC -c "$G/tos/hostfs.c" -o "$O/hostfs.o"
"$BIN/m68k-elf-as" -m68040 --register-prefix-optional -o "$O/tosml.o" "$G/tos/tosml.s"
"$BIN/m68k-elf-ld" --no-warn-rwx-segments -N -Ttext=0xfa0000 -o "$O/tosml.elf" "$O/tosml.o" "$O/hostfs.o"
end=$("$BIN/m68k-elf-nm" "$O/tosml.elf" | awk '$3 == "_end" { print $1 }')
[ $((0x$end)) -le $((0xfa0000 + 0x20000)) ] || { echo "[FAIL] cartridge ends at $end" >&2; exit 1; }
# UTEST.PRG: linked at 0 and 0x10000, the difference gives the relocations
$XCC -c "$T/tos/utest.c" -o "$O/utest.o"
"$BIN/m68k-elf-as" -m68040 --register-prefix-optional -o "$O/gem.o" "$T/tos/gem.s"
"$BIN/m68k-elf-as" -m68040 --register-prefix-optional -o "$O/irq.o" "$T/tos/irq.s"
for base in 0 0x10000; do
	"$BIN/m68k-elf-ld" --no-warn-rwx-segments -N -Ttext=$base -o "$O/u$base.elf" "$O/gem.o" "$O/utest.o" "$O/irq.o"
	"$BIN/m68k-elf-objcopy" -O binary "$O/u$base.elf" "$O/u$base.bin"
done
end=$("$BIN/m68k-elf-nm" "$O/u0.elf" | awk '$3 == "_end" { print $1 }')
python3 "$T/tos/elf2prg.py" "$O/u0.bin" "$O/u0x10000.bin" \
	$((0x$end - $(wc -c < "$O/u0.bin"))) "$O/utest.prg"
"$BIN/m68k-elf-objcopy" -O binary "$O/tosml.elf" "$R/etc/tos/tosml.img"
# the system C: folder: C:\AUTO\UTEST.PRG, G: in its drive table
mkdir -p "$R/tos/sys/AUTO"
cp "$O/utest.prg" "$R/tos/sys/AUTO/UTEST.PRG"
printf 'Drive C: of the TOS container.\r\n' > "$R/tos/sys/README.TXT"
printf '# system drives\nG /tmp/tosg ro\n' > "$R/tos/sys/drives"
# fVDI's files for C: (FVDI.SYS, ASHFB.SYS, AUTO\FVDI.PRG): GEM on the frame buffer
if AUX=$AUX sh "$G/tos/fvdi/build.sh" "$O/fvdi" > "$O/fvdi.log" 2>&1; then
	mkdir -p "$R/tos/fvdi/AUTO"
	cp "$O/fvdi/fvdi.sys" "$R/tos/fvdi/FVDI.SYS"
	cp "$O/fvdi/ashfb.sys" "$R/tos/fvdi/ASHFB.SYS"
	cp "$O/fvdi/fvdi.prg" "$R/tos/fvdi/AUTO/FVDI.PRG"
else
	echo "[skip] fVDI: $(tail -1 "$O/fvdi.log")"
fi
# TeraDesk, started by the desktop
if AUX=$AUX MINT=$O/mint sh "$G/tos/teradesk/build.sh" "$O/teradesk" > "$O/teradesk.log" 2>&1; then
	mkdir -p "$R/tos/teradesk"
	cp -r "$O/teradesk/TERADESK" "$R/tos/teradesk/"
else
	echo "[skip] TeraDesk: $(tail -1 "$O/teradesk.log")"
fi
rm -rf "$O/mint"
# Ballerburg, when its files are at hand: GEM, plus direct screen writes
if [ -f "$AUX/ref/tosapps/baller/BALLER.PRG" ]; then
	mkdir -p "$R/tos/baller"
	cp "$AUX"/ref/tosapps/baller/BALLER.* "$R/tos/baller/"
fi
echo "[ok] starttos, cartridge, C: folder, $(ls "$R/etc/tos" | grep -c 'rom\|emutos') ROM images"
