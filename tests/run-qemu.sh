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
# (QEMU -g: WxHxDEPTH), CMDLINE (one boot option) override.
# Direct boot also serves t_display's host requests on SCC channel B
# (display/hostio.py: keys, mouse, screen dumps in results/.../display/).
# Out: results/<time>-<mode>/ with serial.log, serial.ts (host time of each
# line), screen.png, summary.txt.
# GROUP=smoke|core|mac|tos|mint|amiga|display runs that set only.
# Exit: 0 all PASS, 1 any FAIL, 2 timeout, panic or no TESTS DONE.
# up to QSLOTS QEMUs at once (tools/qslot.sh)
# one run per checkout at a time: they share tests/build
[ -n "$AUX_TLOCK" ] || AUX_TLOCK=1 exec flock "$(dirname "$0")/.treelock" sh "$0" "$@"
[ -n "$AUX_QLOCK" ] || exec sh "$(dirname "$0")/../tools/qslot.sh" "$0" "$@"
T=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$T/.." && pwd)
Q=$AUX/toolchain/qemu-local; [ -x "$Q/usr/bin/qemu-system-m68k" ] || Q=$AUX/toolchain/qemu
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
TMO=${TMO:-3600}
OUT=$T/results/$(date +%Y%m%d-%H%M%S)-$MODE
mkdir -p "$OUT"

# GROUP: a named set of test programs (ONLY= lists them explicitly)
case ${GROUP:-} in
"") ;;
smoke)	G="t_sys t_file t_proc t_pipe t_dlm t_gate t_vtop" ;;
core)	G="t_arith t_file t_mem t_pipe t_proc t_sig t_streams t_sys t_time t_tty t_vtop t_dlm t_gate t_page t_moddemo t_stress t_env t_ufs" ;;
mac)	G="t_aux t_mac t_mac6 t_mac76 t_mac81 t_env" ;;
tos)	G="t_tos t_env" ;;
mint)	G="t_mint" ;;
amiga)	G="t_amiga t_env" ;;
display) G="t_display" ;;
*)	echo "GROUP: smoke core mac tos mint amiga display"; exit 2 ;;
esac
if [ -n "${G:-}" ]; then
	ONLY="$T/src/runall.c"
	for t in $G; do ONLY="$ONLY $T/src/$t.c"; done
	export ONLY
fi
# keep the newest dozen result directories
ls -dt "$T"/results/*/ 2>/dev/null | tail -n +13 | while read d; do rm -rf "$d"; done

# a kernel older than its sources would test yesterday's kernel
NEWER=$(find "$KDIR/mac" "$KDIR/dlm" "$KDIR/tools" "$KDIR/port-local.diff" \
	"$KDIR/amix-040-060-port/src" -newer "$KERNEL" -type f \
	\( -name '*.[chs]' -o -name '*.sh' -o -name '*.py' -o -name '*.diff' -o -name '*.ld' \) \
	! -path '*/build/*' ! -path '*/test/*' 2>/dev/null | head -3)
if [ -n "$NEWER" ] && [ -z "$STALE_OK" ]; then
	echo "[FAIL] $KERNEL is older than:"; echo "$NEWER"
	echo "run sh kernel/build.sh (or STALE_OK=1 to test it anyway)"; exit 2
fi
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

# sockets in a short directory: a unix socket path holds at most 107 bytes
SK=$(mktemp -d /tmp/aq.XXXXXX) || exit 2
trap 'rm -rf "$SK"' EXIT
trap 'exit 2' HUP INT TERM
SOCK=$SK/qmp.sock
QEMU="$Q/usr/bin/qemu-system-m68k -L $Q/usr/share/qemu -M q800 -display none"
# GEOM: the screen, e.g. 800x600x1 for a 1-bit display
[ -n "$GEOM" ] && QEMU="$QEMU -g $GEOM"
# CMDLINE: one kernel boot option for a direct boot, e.g. nofpu
[ -n "$CMDLINE" ] && [ $MODE != rom ] && QEMU="$QEMU -append $CMDLINE"
# GDB: a unix socket path for a gdbstub (the guest runs without waiting)
[ -n "$GDB" ] && QEMU="$QEMU -gdb unix:$GDB,server=on,wait=off"
if [ $MODE = direct ]; then
	MEM=${MEM:-128}
	set -- -m "$MEM" -kernel "$KERNEL" -initrd "$T/build/testroot.img"
	# t_ufs's volume, writes discarded
	[ -f "$T/build/ufs/ufs.img" ] && set -- "$@" \
		-drive file="$T/build/ufs/ufs.img",format=raw,if=none,id=hd2,snapshot=on \
		-device scsi-hd,scsi-id=2,drive=hd2
	# t_amiga's SYS: volume, writes discarded
	[ -f "$T/build/amiga/sys.img" ] && set -- "$@" \
		-drive file="$T/build/amiga/sys.img",format=raw,if=none,id=hd0,snapshot=on \
		-device scsi-hd,scsi-id=0,drive=hd0
	# the Mac System Folders' volume, writes discarded
	[ -f "$T/build/aux/macsys.img" ] && set -- "$@" \
		-drive file="$T/build/aux/macsys.img",format=raw,if=none,id=hd1,snapshot=on \
		-device scsi-hd,scsi-id=1,drive=hd1
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
	SERB="-chardev socket,id=serb,path=$SK/serb.sock,server=on,wait=off,logfile=$OUT/serial-b.log
		-serial chardev:serb -qmp unix:$SK/qmp-ds.sock,server=on,wait=off"
fi
echo "[*] $MODE boot, $MEM MB, timeout $TMO s -> $OUT"
LD_LIBRARY_PATH=$Q/lib timeout "$TMO" nice -n 19 $QEMU "$@" \
	-serial file:"$OUT/serial.log" $SERB \
	-qmp unix:"$SOCK",server=on,wait=off &
QPID=$!
HPID=
if [ $MODE = direct ]; then
	nice -n 19 python3 "$T/display/hostio.py" "$OUT" "$SK" 2> "$OUT/hostio.err" &
	HPID=$!
fi
# host timestamp per console line, to check the guest clock against
tail --pid=$QPID -s 0.2 -F "$OUT/serial.log" 2>/dev/null | while IFS= read -r l; do
	echo "$(date +%s.%N | cut -c1-14) $l"
done > "$OUT/serial.ts" &

t0=$(date +%s)
state=timeout
while kill -0 $QPID 2>/dev/null; do
	# then a last stage that halts, if announced
	if grep -q '^TESTS DONE' "$OUT/serial.log" 2>/dev/null &&
	    { ! grep -q '^HALT NEXT' "$OUT/serial.log" ||
	    grep -q '^HALT END\|system is halted' "$OUT/serial.log"; }; then
		state=done; break
	fi
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

python3 "$T/summarize.py" "$OUT/serial.log" "$state" > "$OUT/summary.txt"
rc=$?
cat "$OUT/summary.txt"
echo "results: $OUT"
exit $rc
