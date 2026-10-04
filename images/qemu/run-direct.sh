#!/bin/sh
# Direct-boot the kernel ELF on QEMU q800 (no ROM), headless.
# usage: run-direct.sh [OUTDIR] [QMP-STEP...]   (steps: see qmp.py)
# Default steps: screendumps while booting, type "echo hello", registers.
# one QEMU at a time on this machine: wait for the lock
[ -n "$AUX_QLOCK" ] || { mkdir -p "$(dirname "$0")/../../images/work" 2>/dev/null; AUX_QLOCK=1 exec flock "$(cd "$(dirname "$0")/../.." && pwd)/images/work/.qemu.lock" sh "$0" "$@"; }
HERE=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$HERE/../.." && pwd)
Q=$AUX/toolchain/qemu
KERNEL=${KERNEL:-$AUX/kernel/build/unix-mac.elf}
OUT=${1:-$HERE/results/direct}
[ $# -gt 0 ] && shift
mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT"/serial*.log "$OUT/monitor.txt"
SOCK=$OUT/qmp.sock
rm -f "$SOCK" "$OUT/serial.sock"

LD_LIBRARY_PATH=$Q/lib timeout 120 nice -n 19 \
  "$Q/usr/bin/qemu-system-m68k" -L "$Q/usr/share/qemu" \
  -M q800 -m 128 -kernel "$KERNEL" \
  -display none \
  -chardev socket,id=ser0,path="$OUT/serial.sock",server=on,wait=off,logfile="$OUT/serial.log" \
  -serial chardev:ser0 -serial file:"$OUT/serial-b.log" \
  -qmp unix:"$SOCK",server=on,wait=off &
QPID=$!

if [ $# -eq 0 ]; then
  set -- wait:4 shot:t04 wait:11 shot:t15 \
    'type:echo hello\n' wait:3 shot:typed 'type:ls /\n' wait:4 shot:ls \
    'hmp:info registers' 'hmp:x /16i $pc' quit
fi
python3 "$HERE/qmp.py" "$SOCK" "$OUT" "$@"
wait $QPID
echo "serial log: $OUT/serial.log"
