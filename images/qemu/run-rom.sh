#!/bin/sh
# Try the q800 machine with a ROM image (-bios); optional SCSI disk overlay.
# usage: run-rom.sh ROMFILE [DISKIMG] [OUTDIR]
HERE=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$HERE/../.." && pwd)
Q=$AUX/toolchain/qemu-local; [ -x "$Q/usr/bin/qemu-system-m68k" ] || Q=$AUX/toolchain/qemu
ROM=$1
DISK=$2
OUT=${3:-$HERE/results/rom}
mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT/serial.log" "$OUT/qmp.sock" "$OUT/monitor.txt"
DRIVE=
if [ -n "$DISK" ]; then
  # -snapshot: writes go to a temporary overlay, the image is not modified
  DRIVE="-snapshot -drive file=$DISK,format=raw,if=none,id=hd0 -device scsi-hd,scsi-id=0,drive=hd0"
fi
LD_LIBRARY_PATH=$Q/lib timeout 60 nice -n 19 \
  "$Q/usr/bin/qemu-system-m68k" -L "$Q/usr/share/qemu" \
  -M q800 -m 128 -bios "$ROM" $DRIVE \
  -display none -serial file:"$OUT/serial.log" \
  -qmp unix:"$OUT/qmp.sock",server=on,wait=off &
QPID=$!
python3 "$HERE/qmp.py" "$OUT/qmp.sock" "$OUT" wait:5 shot:t05 wait:5 shot:t10 \
  wait:10 shot:t20 'hmp:info registers' quit
wait $QPID
