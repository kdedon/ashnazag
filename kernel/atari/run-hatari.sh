#!/bin/sh
# Run a Falcon in Hatari for a fixed number of frames and save a screenshot.
#
#   run-hatari.sh [options] [-- extra hatari options]
#     -c 030|060    CPU: 68030 without FPU (default) or 68060 with FPU (CT60)
#     -s MB         ST-RAM, 0-14 (default 14)
#     -t MB         TT-RAM/FastRAM, multiple of 4 (default 0)
#     -d IMAGE      hard disk image (writable: pass a copy)
#     -b ide|scsi|acsi   bus for -d (default ide)
#     -r ROM        TOS image (default: EmuTOS 512k us)
#     -f FRAMES     screenshot and quit after this many VBLs (default 1500)
#     -o FILE       screenshot, .png (default ./hatari-shot.png)
#     -T SECONDS    wall-clock limit (default 300)
#     -l FILE       Hatari log (default: next to the screenshot, .log)
#     -k ELF        boot this kernel directly (Linux/m68k boot record)
#     -a ARGS       kernel command line for -k
#     -K SCRIPT     drive the keyboard with hatari-keys.py (waits on the
#                   console copy: give -a nfcons)
#
# Runs fast-forward, without sound or a window.  Config, NVRAM and the
# EmuTOS copy live in a throw-away directory, never in $HOME.
set -e

AUX=$(cd "$(dirname "$0")/../.." && pwd)
HATARI=${HATARI:-$AUX/ref/hatari/build/src/hatari}
CPU=030 ST=14 TT=0 DISK= BUS=ide ROM= FRAMES=1500 OUT=./hatari-shot.png TMO=300 LOG=
KERNEL= KARGS= KEYS=
while getopts c:s:t:d:b:r:f:o:T:l:k:a:K: o; do
	case $o in
	c) CPU=$OPTARG ;; s) ST=$OPTARG ;; t) TT=$OPTARG ;; d) DISK=$OPTARG ;;
	b) BUS=$OPTARG ;; r) ROM=$OPTARG ;; f) FRAMES=$OPTARG ;; o) OUT=$OPTARG ;;
	T) TMO=$OPTARG ;; l) LOG=$OPTARG ;; k) KERNEL=$OPTARG ;; a) KARGS=$OPTARG ;;
	K) KEYS=$OPTARG ;;
	*) sed -n '2,20p' "$0"; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ "$1" = -- ] && shift
[ -x "$HATARI" ] || { echo "no hatari at $HATARI" >&2; exit 1; }

case $OUT in /*) ;; *) OUT=$PWD/$OUT ;; esac
LOG=${LOG:-${OUT%.*}.log}
RUN=$(mktemp -d "${TMPDIR:-/tmp}/hatari.XXXXXX")
trap 'rm -rf "$RUN"' EXIT

if [ -z "$ROM" ]; then
	ROM=$RUN/etos512us.img
	unzip -p "$AUX/ref/emutos-release/emutos-512k-1.4.zip" \
		emutos-512k-1.4/etos512us.img > "$ROM"
fi

case $CPU in
030) CPUOPT="--cpulevel 3 --fpu none" ;;
060) CPUOPT="--cpulevel 6 --fpu internal" ;;
*) echo "cpu: 030 or 060" >&2; exit 2 ;;
esac

DISKOPT=
if [ -n "$DISK" ]; then
	case $BUS in
	ide) DISKOPT="--ide-master $DISK" ;;
	scsi) DISKOPT="--scsi 0=$DISK" ;;
	acsi) DISKOPT="--acsi 0=$DISK" ;;
	*) echo "bus: ide, scsi or acsi" >&2; exit 2 ;;
	esac
fi

if [ -n "$KERNEL" ]; then
	case $KERNEL in /*) ;; *) KERNEL=$PWD/$KERNEL ;; esac
	mkdir -p "$RUN/.config/hatari"
	printf '[LILO]\nArgs =\nKernel = %s\nRamdisk = \nSymbols = \nKernelToFastRam = FALSE\nRamdiskToFastRam = FALSE\nHaltOnReboot = TRUE\n' \
		"$KERNEL" > "$RUN/.config/hatari/hatari.cfg"
	set -- --natfeats yes --lilo "${KARGS:- }" "$@"
fi

# At frame FRAMES the debugger takes the screenshot and quits.
printf 'screenshot %s\nquit 0\n' "$OUT" > "$RUN/shot.ini"
printf 'b VBL = %d :once :file %s\n' "$FRAMES" "$RUN/shot.ini" > "$RUN/init.ini"

if [ -n "$KEYS" ]; then
	[ -f "$KEYS" ] || { echo "no key script $KEYS" >&2; exit 2; }
	set -- --cmd-fifo "$RUN/fifo" "$@"
	rm -f "$LOG.out"
	python3 "$(dirname "$0")/hatari-keys.py" "$RUN/fifo" "$LOG.out" < "$KEYS" &
	KPID=$!
fi

rm -f "$OUT"
# line-buffered stdout: the boot loader's messages come before the
# console's, not appended at exit as if the machine had rebooted
HOME=$RUN SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
timeout -k 5 "$TMO" nice -n 19 stdbuf -oL "$HATARI" \
	--machine falcon $CPUOPT --mmu on --addr24 off \
	--memsize "$ST" --ttram "$TT" --dsp none \
	--tos "$ROM" --monitor vga $DISKOPT \
	--fast-forward on --sound off --confirm-quit no \
	--statusbar off --drive-led off --crop on \
	--parse "$RUN/init.ini" --log-file "$LOG" "$@" \
	< /dev/null > "$LOG.out" 2>&1 || rc=$?
if [ -n "$KEYS" ]; then
	kill "$KPID" 2>/dev/null && echo "key script did not finish" >&2
	wait "$KPID" || krc=$?
	[ "${krc:-0}" -eq 0 ] || [ "${krc:-0}" -eq 143 ] || { echo "key script failed" >&2; exit 1; }
fi
[ -s "$OUT" ] || { echo "no screenshot (rc ${rc:-0}); see $LOG.out" >&2; exit 1; }
echo "$OUT"
