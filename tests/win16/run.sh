#!/bin/sh
# run.sh -- the Win16 environment on this host: startwin built with the
# host's cc (in-memory screen), the test programs in src/ built with Open
# Watcom, each run with its input script; what it reports (RESULT.TXT)
# and its exit code are checked against expect/.
#
#   sh tests/win16/run.sh [program...]
#
# WATCOM names an Open Watcom 2 install (default toolchain/watcom); without
# one the programs are not built and only startwin is.  Screenshots the
# scripts take go to tests/build/win16/shots.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
G=$AUX/kernel/guest/win16
B=$T/build/win16
WATCOM=${WATCOM:-$AUX/toolchain/watcom}
mkdir -p "$B/obj" "$B/progs" "$B/shots"
for f in "$G"/*.c; do
	case $(basename "$f") in scr_fb.c|snd_so.c) continue ;; esac
	o=$B/obj/$(basename "$f" .c).o
	cc -std=gnu89 -O1 -w -I"$G" -c -o "$o" "$f"
done
cc -o "$B/startwin" "$B"/obj/*.o -lm
echo "[ok] startwin for this host"
# -install from setup disks: SETUP.INF placing the files, SZDD expanded,
# our own modules (USER.EXE) left out
I=$B/install
rm -rf "$I"
mkdir -p "$I/d/DISK1" "$I/d/DISK2"
head -c 3000 "$G/user.c" > "$I/CALC.EXE"
head -c 999 "$G/gdi.c" > "$I/COMMDLG.DLL"
printf 'x' > "$I/USER.EXE"
printf '[windows]\r\n2:calc.exe, "Calculator"\r\n[windows.system]\r\n1:commdlg.dll\r\n1:user.exe\r\n' \
	> "$I/d/DISK1/SETUP.INF"
python3 "$T/win16/szdd.py" "$I/CALC.EXE" "$I/d/DISK2/CALC.EX_"
python3 "$T/win16/szdd.py" "$I/COMMDLG.DLL" "$I/d/DISK1/COMMDLG.DL_"
python3 "$T/win16/szdd.py" "$I/USER.EXE" "$I/d/DISK1/USER.EX_"
"$B/startwin" -C "$I/c" -install "$I/d" > "$I/log"
cmp -s "$I/CALC.EXE" "$I/c/windows/calc.exe" && cmp -s "$I/COMMDLG.DLL" "$I/c/windows/system/commdlg.dll" &&
	[ ! -f "$I/c/windows/system/user.exe" ] && [ -f "$I/c/windows/system.ini" ] ||
	{ cat "$I/log"; echo "[FAIL] startwin -install"; exit 1; }
echo "[ok] startwin -install"
if [ ! -x "$WATCOM/binl64/wcl" ] && [ ! -x "$WATCOM/binl/wcl" ]; then
	echo "[skip] no Open Watcom in $WATCOM: the test programs are not built"
	exit 0
fi
BIN=$WATCOM/binl64
[ -x "$BIN/wcl" ] || BIN=$WATCOM/binl
export WATCOM INCLUDE="$WATCOM/h:$WATCOM/h/win" PATH="$BIN:$PATH"
cd "$T/win16/src"
for c in *.c; do
	n=${c%.c}
	fp=
	[ $n != fpu ] || fp=-fpi87	# the x87's own instructions, not the emulator's calls
	lib=
	[ $n != sock ] || lib="-\"library winsock\""	# WINSOCK's imports
	wcl -q -bt=windows -l=windows -ms -zW $fp -fe="$B/progs/$n.exe" -fo="$B/progs/$n.obj" $c $lib
	if [ -f $n.rc ]; then
		wrc -q -bt=windows -r -fo="$B/progs/$n.res" $n.rc
		wrc -q -bt=windows "$B/progs/$n.res" "$B/progs/$n.exe"
	fi
done
echo "[ok] test programs: $(ls *.c | wc -l | tr -d ' ')"
# program, expected exit code
fails=0
for t in "hello 7" "menus 0" "ctrls 1" "fpu 0" "voice 0" "sock 0"; do
	set -- $t
	n=$1
	want=$2
	[ $# -gt 0 ] || continue
	rm -rf "$B/c"
	mkdir -p "$B/c"
	cp "$B/progs/$n.exe" "$B/c/"
	cd "$B/shots"
	rm -f "$B/$n.snd"
	set +e
	W16_SND=$B/$n.snd timeout 60 "$B/startwin" -C "$B/c" -S "$T/win16/scripts/$n.scr" "$B/c/$n.exe" > "$B/$n.log" 2>&1
	code=$?
	set -e
	ok=1
	# the voice's samples (8-bit 11025 Hz): 1125 ms of them, the first note A at 440 Hz
	if [ $n = voice ]; then
		python3 - "$B/$n.snd" <<'PY' || ok=0
import sys
d = open(sys.argv[1], 'rb').read()
x = sum(1 for i in range(1, 5512) if (d[i] >= 128) != (d[i - 1] >= 128))
rest = all(b == 0x80 for b in d[5600:8000])
sys.exit(0 if abs(len(d) - 12403) < 400 and 425 <= x <= 455 and rest else 1)
PY
	fi
	[ "$code" = "$want" ] || ok=0
	if [ -f "$T/win16/expect/$n.txt" ]; then
		tr -d '\r' < "$B/c/result.txt" > "$B/$n.result" 2> /dev/null || : > "$B/$n.result"
		cmp -s "$B/$n.result" "$T/win16/expect/$n.txt" || ok=0
	fi
	if [ $ok = 1 ]; then
		echo "[ok] $n"
	else
		echo "[FAIL] $n: exit $code (want $want); see $B/$n.log, $B/$n.result"
		fails=$((fails + 1))
	fi
done
[ $fails = 0 ]
