#!/bin/sh
# run-qemu.sh -- boot the test root on a Quadra 800 model and collect results.
#
#   sh tests/run-qemu.sh [--rom | --net] [--no-build] [--kernel-dir DIR]
#
# Default: direct boot of KERNEL with the test root as the RAM-disk boot
# record, so the kernel under test is unchanged.  --rom: ROM boot through
# A/UX Startup from mkromimage.sh's disk.  --net: direct boot of the
# network root (t_net only) on user-mode networking, with a TCP echo
# service (cat) at 10.0.2.100:7.  --kernel-dir: a kernel/ tree
# (built) to test instead of ../kernel.  ROM, MEM (MB), TMO (s), GEOM
# (QEMU -g: WxHxDEPTH) override.
# Direct boot also serves t_display's host requests on SCC channel B
# (display/hostio.py: keys, mouse, screen dumps in results/.../display/).
# Out: results/<time>-<mode>/ with serial.log, serial.ts (host time of each
# line), screen.png, summary.txt.
# Exit: 0 all PASS, 1 any FAIL, 2 timeout, panic or no TESTS DONE.
T=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$T/.." && pwd)
Q=$AUX/toolchain/qemu
MODE=direct
BUILD=1
KDIR=$AUX/kernel
while [ $# -gt 0 ]; do
	case $1 in
	--rom) MODE=rom ;;
	--net) MODE=net ;;
	--no-build) BUILD= ;;
	--kernel-dir) KDIR=$(cd "$2" && pwd) || exit 2; shift ;;
	*) echo "usage: run-qemu.sh [--rom | --net] [--no-build] [--kernel-dir DIR]"; exit 2 ;;
	esac
	shift
done
export KDIR
KERNEL=${KERNEL:-$KDIR/build/unix-mac.elf}
ROM=${ROM:-$AUX/Quadra 800.ROM}
TMO=${TMO:-300}
OUT=$T/results/$(date +%Y%m%d-%H%M%S)-$MODE
mkdir -p "$OUT"

if [ -n "$BUILD" ]; then
	sh "$T/build.sh" > "$OUT/build.log" 2>&1 || { tail "$OUT/build.log"; exit 2; }
	if [ $MODE = direct ]; then
		KERNEL=$KERNEL sh "$T/mktestroot.sh" >> "$OUT/build.log" 2>&1 ||
			{ tail "$OUT/build.log"; exit 2; }
	elif [ $MODE = net ]; then
		NET=1 KERNEL=$KERNEL sh "$T/mktestroot.sh" >> "$OUT/build.log" 2>&1 ||
			{ tail "$OUT/build.log"; exit 2; }
	else
		sh "$T/mkromimage.sh" >> "$OUT/build.log" 2>&1 || { tail "$OUT/build.log"; exit 2; }
	fi
fi

SOCK=$OUT/qmp.sock
QEMU="$Q/usr/bin/qemu-system-m68k -L $Q/usr/share/qemu -M q800 -display none"
# GEOM: the screen, e.g. 800x600x1 for a 1-bit display
[ -n "$GEOM" ] && QEMU="$QEMU -g $GEOM"
if [ $MODE = direct ]; then
	MEM=${MEM:-128}
	set -- -m "$MEM" -kernel "$KERNEL" -initrd "$T/build/testroot.img"
elif [ $MODE = net ]; then
	MEM=${MEM:-128}
	set -- -m "$MEM" -kernel "$KERNEL" -initrd "$T/build/netroot.img" \
		-nic user,guestfwd=tcp:10.0.2.100:7-cmd:cat
else
	MEM=${MEM:-32}
	DISK=$T/build/rom/q800-test-small-tests.img
	[ -f "$ROM" ] || { echo "no ROM: $ROM"; exit 2; }
	set -- -m "$MEM" -bios "$ROM" -snapshot \
		-drive file="$DISK",format=raw,if=none,id=hd0 -device scsi-hd,scsi-id=0,drive=hd0
fi
SERB="-serial file:$OUT/serial-b.log"
if [ $MODE = direct ]; then
	SERB="-chardev socket,id=serb,path=$OUT/serb.sock,server=on,wait=off,logfile=$OUT/serial-b.log
		-serial chardev:serb -qmp unix:$OUT/qmp-ds.sock,server=on,wait=off"
fi
echo "[*] $MODE boot, $MEM MB, timeout $TMO s -> $OUT"
LD_LIBRARY_PATH=$Q/lib timeout "$TMO" nice -n 19 $QEMU "$@" \
	-serial file:"$OUT/serial.log" $SERB \
	-qmp unix:"$SOCK",server=on,wait=off &
QPID=$!
HPID=
if [ $MODE = direct ]; then
	nice -n 19 python3 "$T/display/hostio.py" "$OUT" 2> "$OUT/hostio.err" &
	HPID=$!
fi
# host timestamp per console line, to check the guest clock against
tail --pid=$QPID -s 0.2 -F "$OUT/serial.log" 2>/dev/null | while IFS= read -r l; do
	echo "$(date +%s.%N | cut -c1-14) $l"
done > "$OUT/serial.ts" &

t0=$(date +%s)
state=timeout
while kill -0 $QPID 2>/dev/null; do
	if grep -q '^TESTS DONE' "$OUT/serial.log" 2>/dev/null; then state=done; break; fi
	if grep -qi 'panic' "$OUT/serial.log" 2>/dev/null; then state=panic; sleep 3; break; fi
	sleep 2
done
[ $state = timeout ] && ! kill -0 $QPID 2>/dev/null && [ $(( $(date +%s) - t0 )) -lt "$TMO" ] &&
	state=exited
if kill -0 $QPID 2>/dev/null; then
	sleep 1
	nice -n 19 python3 "$AUX/images/qemu/qmp.py" "$SOCK" "$OUT" shot:screen \
		'hmp:info registers' quit > "$OUT/qmp.log" 2>&1
	sleep 2
	kill $QPID 2>/dev/null
fi
[ -n "$HPID" ] && kill $HPID 2>/dev/null
wait
rm -f "$SOCK" "$OUT/serb.sock" "$OUT/qmp-ds.sock"

python3 "$T/summarize.py" "$OUT/serial.log" "$state" > "$OUT/summary.txt"
rc=$?
cat "$OUT/summary.txt"
echo "results: $OUT"
exit $rc
