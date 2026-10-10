#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
LX=$AUX/toolchain/linux/bin/m68k-linux-gnu
TB=$AUX/toolchain/bin
"$TB/m68k-elf-as" -m68020 --defsym ENET_MODE=2 -o "$OUT/enet.o" "$AUX/kernel/otbridge/n0n2/enet/enet.s"
"$TB/m68k-elf-objcopy" -O binary -j .text "$OUT/enet.o" "$OUT/enet.drvr"
(cd "$OUT" && "$LX-as" -m68020 -o netinit.o "$D/netinit.s")
"$LX-ld" -Ttext=0 -e entry --emit-relocs --build-id=none -o "$OUT/netinit.elf" "$OUT/netinit.o"
"$LX-objcopy" -O binary -j .text "$OUT/netinit.elf" "$OUT/netinit.bin"
python3 "$D/check.py" "$OUT"
python3 "$D/mkinit.py" "$OUT/netinit.bin" "$OUT"
mkdir -p "$OUT/Extensions"
cp "$OUT/AUXNet" "$OUT/%AUXNet" "$OUT/Extensions/"
PROVIDER_LIBRARY='OTLib$hostinet' PROVIDER_FILENAME='AUX Host Internet Library' \
    sh "$AUX/kernel/otbridge/relay/build-provider.sh" "$OUT/provider"
sh "$AUX/kernel/otbridge/relay/build-configurator.sh" "$OUT/configurator"
for library in "$OUT/provider/AUX Host Internet Library" "$OUT/configurator/AUX Host Configurator Library"; do
    python3 "$AUX/tools/macbin2ad.py" "$library" "$OUT/Extensions"
done
python3 "$D/test-stage.py" "$OUT"
