#!/bin/sh
# build.sh -- t_tos's inputs: starttos, the machine-layer cartridge, a
# 2 MB FAT drive C:, EmuTOS and the user's TOS ROM image.
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
"$BIN/m68k-elf-as" -m68040 --register-prefix-optional -o "$O/tosml.o" "$G/tos/tosml.s"
"$BIN/m68k-elf-ld" -Ttext=0xfa0000 -o "$O/tosml.elf" "$O/tosml.o"
"$BIN/m68k-elf-objcopy" -O binary "$O/tosml.elf" "$R/etc/tos/tosml.img"
MKFS=$(command -v mkfs.fat || echo /usr/sbin/mkfs.fat)
"$MKFS" -C -F 12 -s 2 -S 512 -n TOSC "$R/etc/tos/c.img" 2048 > /dev/null
# one file in the root: README.TXT in cluster 2
python3 - "$R/etc/tos/c.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
b = f.read(512)
bps, spc, res, nfat, root = struct.unpack_from('<HBHBH', b, 11)
fsz = struct.unpack_from('<H', b, 22)[0]
rootsec = res + nfat * fsz
data = rootsec + root * 32 // bps
text = b'Drive C: of the TOS container.\r\n'
for k in range(nfat):			# FAT12 entry 2: end of chain
    f.seek((res + k * fsz) * bps + 3)
    f.write(bytes([0xff, 0x0f]))
f.seek(rootsec * bps + 32)		# after the volume label
f.write(b'README  TXT' + bytes([0x20]) + bytes(14) + struct.pack('<HI', 2, len(text)))
f.seek(data * bps)
f.write(text)
PY
echo "[ok] starttos, cartridge, drive C:, $(ls "$R/etc/tos" | grep -c 'rom\|emutos') ROM images"
