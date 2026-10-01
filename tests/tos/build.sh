#!/bin/sh
# build.sh -- t_tos's inputs: starttos, the machine-layer cartridge, a
# 2 MB FAT drive C: with C:\AUTO\UTEST.PRG (checks drive U:), EmuTOS and
# the user's TOS ROM image.
#
#   sh tests/tos/build.sh outdir
#
# Out: outdir/root/tos/bin/starttos, outdir/root/etc/tos/{emutos.img,rom,
# tosml.img,c.img}.
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
for base in 0 0x10000; do
	"$BIN/m68k-elf-ld" --no-warn-rwx-segments -N -Ttext=$base -o "$O/u$base.elf" "$O/gem.o" "$O/utest.o"
	"$BIN/m68k-elf-objcopy" -O binary "$O/u$base.elf" "$O/u$base.bin"
done
end=$("$BIN/m68k-elf-nm" "$O/u0.elf" | awk '$3 == "_end" { print $1 }')
python3 "$T/tos/elf2prg.py" "$O/u0.bin" "$O/u0x10000.bin" \
	$((0x$end - $(wc -c < "$O/u0.bin"))) "$O/utest.prg"
"$BIN/m68k-elf-objcopy" -O binary "$O/tosml.elf" "$R/etc/tos/tosml.img"
MKFS=$(command -v mkfs.fat || echo /usr/sbin/mkfs.fat)
"$MKFS" -C -F 12 -s 2 -S 512 -n TOSC "$R/etc/tos/c.img" 2048 > /dev/null
# README.TXT in cluster 2, AUTO in 3, AUTO\UTEST.PRG from 4 on
python3 - "$R/etc/tos/c.img" "$O/utest.prg" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
prg = open(sys.argv[2], 'rb').read()
b = f.read(512)
bps, spc, res, nfat, root = struct.unpack_from('<HBHBH', b, 11)
fsz = struct.unpack_from('<H', b, 22)[0]
rootsec = res + nfat * fsz
data = rootsec + root * 32 // bps
text = b'Drive C: of the TOS container.\r\n'
csz = bps * b[13]
n = (len(prg) + csz - 1) // csz
f.seek(res * bps)
fat = bytearray(f.read(fsz * bps))
def link(c, v):
    o = c * 3 // 2
    if c & 1:
        fat[o] = fat[o] & 0x0f | (v << 4) & 0xf0
        fat[o + 1] = v >> 4
    else:
        fat[o] = v & 0xff
        fat[o + 1] = fat[o + 1] & 0xf0 | v >> 8
link(2, 0xfff)
link(3, 0xfff)
for c in range(4, 4 + n):
    link(c, c + 1 if c < 3 + n else 0xfff)
for k in range(nfat):
    f.seek((res + k * fsz) * bps)
    f.write(fat)
def ent(name, attr, clus, size):
    return name + bytes([attr]) + bytes(14) + struct.pack('<HI', clus, size)
f.seek(rootsec * bps + 32)		# after the volume label
f.write(ent(b'README  TXT', 0x20, 2, len(text)) + ent(b'AUTO       ', 0x10, 3, 0))
f.seek(data * bps)
f.write(text)
f.seek((data + b[13]) * bps)
f.write(ent(b'.          ', 0x10, 3, 0) + ent(b'..         ', 0x10, 0, 0) +
        ent(b'UTEST   PRG', 0x20, 4, len(prg)))
f.seek((data + 2 * b[13]) * bps)
f.write(prg)
PY
echo "[ok] starttos, cartridge, drive C:, $(ls "$R/etc/tos" | grep -c 'rom\|emutos') ROM images"
