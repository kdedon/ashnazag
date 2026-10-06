#!/bin/sh
# build.sh dir [System] -- the Log Out application into dir; with an
# AppleSingle or AppleDouble System file, also its desk accessory into it.
set -e
D=$(cd "$(dirname "$0")" && pwd)
LX=$D/../../../toolchain/linux/bin/m68k-linux-gnu
W=$(mktemp -d)
for n in app da; do
	"$LX-as" -m68020 -o "$W/$n.o" "$D/$n.s"
	"$LX-ld" -e 0 -Ttext=0 --build-id=none -o "$W/$n.elf" "$W/$n.o"
	"$LX-objcopy" -O binary -j .text "$W/$n.elf" "$W/$n.bin"
done
python3 "$D/mklogout.py" "$W/app.bin" "$W/da.bin" "$@"
rm -rf "$W"
