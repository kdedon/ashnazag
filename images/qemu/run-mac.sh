#!/bin/sh
# Boot q800 with a ROM and a SCSI disk (ID 0, -snapshot), screendumps via QMP.
# usage: run-mac.sh ROM DISK MEM_MB OUT [QMP-STEP...]
# Default steps: a screendump every 5 s for 200 s.  GDB=1 adds a gdbstub
# socket OUT/gdb.sock, QEXTRA adds QEMU options.  TMO: hard timeout (260 s).
HERE=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$HERE/../.." && pwd)
Q=$AUX/toolchain/qemu
ROM=$1; DISK=$2; MEM=$3; OUT=$4; shift 4
mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT"/serial*.log "$OUT/monitor.txt" "$OUT/qmp.sock" "$OUT/gdb.sock"
EXTRA=
[ -n "$GDB" ] && EXTRA="-gdb unix:$OUT/gdb.sock,server=on,wait=off"
LD_LIBRARY_PATH=$Q/lib timeout ${TMO:-260} nice -n 19 \
  "$Q/usr/bin/qemu-system-m68k" -L "$Q/usr/share/qemu" \
  -M q800 -m "$MEM" -bios "$ROM" -snapshot \
  -drive file="$DISK",format=raw,if=none,id=hd0 -device scsi-hd,scsi-id=0,drive=hd0 \
  -display none -serial file:"$OUT/serial.log" -serial file:"$OUT/serial-b.log" \
  -qmp unix:"$OUT/qmp.sock",server=on,wait=off $EXTRA $QEXTRA &
QPID=$!
if [ $# -eq 0 ]; then
  t=0
  while [ $t -lt 200 ]; do
    t=$((t + 5)); set -- "$@" wait:5 shot:$(printf 't%03d' $t)
  done
  set -- "$@" 'hmp:info registers' quit
fi
python3 "$HERE/qmp.py" "$OUT/qmp.sock" "$OUT" "$@"
wait $QPID
