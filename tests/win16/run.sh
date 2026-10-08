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
	o=$B/obj/$(basename "$f" .c).o
	cc -std=gnu89 -O1 -w -I"$G" -c -o "$o" "$f"
done
cc -o "$B/startwin" "$B"/obj/*.o -lm
echo "[ok] startwin for this host"
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
	wcl -q -bt=windows -l=windows -ms -zW -fe="$B/progs/$n.exe" -fo="$B/progs/$n.obj" $c
	if [ -f $n.rc ]; then
		wrc -q -bt=windows -r -fo="$B/progs/$n.res" $n.rc
		wrc -q -bt=windows "$B/progs/$n.res" "$B/progs/$n.exe"
	fi
done
echo "[ok] test programs: $(ls *.c | wc -l | tr -d ' ')"
# program, expected exit code
fails=0
for t in "hello 7" "menus 0" "ctrls 1"; do
	set -- $t
	n=$1
	want=$2
	[ $# -gt 0 ] || continue
	rm -rf "$B/c"
	mkdir -p "$B/c"
	cp "$B/progs/$n.exe" "$B/c/"
	cd "$B/shots"
	set +e
	timeout 60 "$B/startwin" -C "$B/c" -S "$T/win16/scripts/$n.scr" "$B/c/$n.exe" > "$B/$n.log" 2>&1
	code=$?
	set -e
	ok=1
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
