#!/bin/sh
# Boot q800 with a ROM and a SCSI disk (ID 0, -snapshot), screendumps via QMP.
# usage: run-mac.sh [--net] [--sock] [--tmo S] ROM DISK MEM_MB OUT [QMP-STEP...]
# --net: user-mode network on the built-in Ethernet.  --sock: serial A on
# socket OUT/serial.sock (for serial: steps), still logged to serial.log.
# --tmo: hard timeout in seconds.
# Default steps: a screendump every 5 s for 200 s.  GDB=1 adds a gdbstub
# socket OUT/gdb.sock, QEXTRA adds QEMU options.  TMO: hard timeout (260 s).
# one QEMU at a time on this machine: wait for the lock
[ -n "$AUX_QLOCK" ] || { mkdir -p "$(dirname "$0")/../../images/work" 2>/dev/null; AUX_QLOCK=1 exec flock "$(cd "$(dirname "$0")/../.." && pwd)/images/work/.qemu.lock" sh "$0" "$@"; }
HERE=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$HERE/../.." && pwd)
Q=$AUX/toolchain/qemu
NET= SERA=
while :; do
  case $1 in
  --net) NET="-nic user"; shift ;;
  --sock) SERA=1; shift ;;
  --tmo) TMO=$2; shift 2 ;;
  *) break ;;
  esac
done
ROM=$1; DISK=$2; MEM=$3; OUT=$4; shift 4
mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT"/serial*.log "$OUT/monitor.txt" "$OUT/qmp.sock" "$OUT/gdb.sock" \
  "$OUT/serial.sock"
SER="-serial file:$OUT/serial.log"
[ -n "$SERA" ] && SER="-chardev socket,id=ser0,path=$OUT/serial.sock,server=on,wait=off,logfile=$OUT/serial.log -serial chardev:ser0"
EXTRA=
[ -n "$GDB" ] && EXTRA="-gdb unix:$OUT/gdb.sock,server=on,wait=off"
LD_LIBRARY_PATH=$Q/lib timeout ${TMO:-260} nice -n 19 \
  "$Q/usr/bin/qemu-system-m68k" -L "$Q/usr/share/qemu" \
  -M q800 -m "$MEM" -bios "$ROM" -snapshot \
  -drive file="$DISK",format=raw,if=none,id=hd0 -device scsi-hd,scsi-id=0,drive=hd0 \
  -display none $SER -serial file:"$OUT/serial-b.log" \
  -qmp unix:"$OUT/qmp.sock",server=on,wait=off $NET $EXTRA $QEXTRA &
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
