#!/bin/sh
# mktos.sh -- the TOS container's files for the disk image: starttos,
# EmuTOS, the machine-layer cartridge and a drive C: image.
#
#   sh images/tosenv/mktos.sh outdir
#
# Out: outdir/{starttos,emutos.img,tosml.img,c.img}.  EMUTOS names the
# EmuTOS 512 KB release zip, EMUTOSLANG its image (us).
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
G=$AUX/kernel/guest
EMUTOS=${EMUTOS:-$AUX/ref/emutos-release/emutos-512k-1.4.zip}
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
BIN=$AUX/toolchain/bin
OUT=$1
[ -n "$OUT" ] || { echo "usage: mktos.sh outdir"; exit 1; }
O=$OUT/obj
rm -rf "$OUT"
mkdir -p "$O/extra"
unzip -p "$EMUTOS" "*/etos512${EMUTOSLANG:-us}.img" > "$OUT/emutos.img"
[ -s "$OUT/emutos.img" ] || { echo "[FAIL] no EmuTOS image in $EMUTOS"; exit 1; }
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
# nonshared libc members
(cd "$O/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
 ar rc ../libextra.a $(ar t "$SYS/usr/ccs/lib/libc.so" | grep -v '^libc.so.1$'))
for c in starttos tosdisp; do
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 \
		-I"$G/mod/tosguest" -I"$AUX/kernel/mac/display" -c "$G/tos/$c.c" -o "$O/$c.o"
done
"$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/starttos" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$O/starttos.o" "$O/tosdisp.o" "$SYS/usr/lib/libc.so.1" \
	"$O/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
# cartridge: machine layer plus the U: drive
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -m68020 -Wall -Wno-implicit -fno-builtin \
	-c "$G/tos/hostfs.c" -o "$O/hostfs.o"
"$BIN/m68k-elf-as" -m68040 --register-prefix-optional -o "$O/tosml.o" "$G/tos/tosml.s"
"$BIN/m68k-elf-ld" --no-warn-rwx-segments -N -Ttext=0xfa0000 -o "$O/tosml.elf" "$O/tosml.o" "$O/hostfs.o"
end=$("$BIN/m68k-elf-nm" "$O/tosml.elf" | awk '$3 == "_end" { print $1 }')
[ $((0x$end)) -le $((0xfa0000 + 0x20000)) ] || { echo "[FAIL] cartridge ends at $end" >&2; exit 1; }
"$BIN/m68k-elf-objcopy" -O binary "$O/tosml.elf" "$OUT/tosml.img"
MKFS=$(command -v mkfs.fat || echo /usr/sbin/mkfs.fat)
"$MKFS" -C -F 12 -s 2 -S 512 -n TOSC "$OUT/c.img" 2048 > /dev/null
# README.TXT in the root, cluster 2
python3 - "$OUT/c.img" "$D/c-readme.txt" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
b = f.read(512)
bps, spc, res, nfat, root = struct.unpack_from('<HBHBH', b, 11)
fsz = struct.unpack_from('<H', b, 22)[0]
rootsec = res + nfat * fsz
data = rootsec + root * 32 // bps
text = open(sys.argv[2], 'rb').read().replace(b'\n', b'\r\n')
assert len(text) <= spc * bps
for k in range(nfat):			# FAT12 entry 2: end of chain
    f.seek((res + k * fsz) * bps + 3)
    f.write(bytes([0xff, 0x0f]))
f.seek(rootsec * bps + 32)		# after the volume label
f.write(b'README  TXT' + bytes([0x20]) + bytes(14) + struct.pack('<HI', 2, len(text)))
f.seek(data * bps)
f.write(text)
PY
rm -rf "$O"
echo "[ok] starttos, EmuTOS, cartridge, drive C:"
