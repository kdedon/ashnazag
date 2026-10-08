#!/bin/sh
# x86suite.sh -- the Win16 environment's x86 core against SingleStepTests'
# 80386 real-mode suite, on this host.
#
#   sh tests/win16/x86suite.sh [FILE.MOO.gz...]
#
# Fetches the suite (MIT, about 1.2 GB) into ref/sst-80386 at a pinned
# commit on first use, builds x86test and runs every opcode file but HLT
# (or the files named).  x86known.txt lists the 35 corner cases we leave.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
G=$AUX/kernel/guest/win16
REF=$AUX/ref/sst-80386
REV=459d49fbe6280e9ed46fee887b58dacd9cb880ab
O=$T/build/win16
mkdir -p "$O"
if [ ! -d "$REF/v1_ex_real_mode" ]; then
	mkdir -p "$AUX/ref"
	git clone -q https://github.com/SingleStepTests/80386 "$REF"
	git -C "$REF" checkout -q $REV
fi
cc -std=gnu89 -O2 -Wall -Wno-implicit-int -Wno-implicit-function-declaration -Wno-parentheses \
	-Wno-unused-variable -I"$G" -o "$O/x86test" "$T/win16/x86test.c" "$G/x86.c" "$G/x87.c"
if [ $# -eq 0 ]; then
	set -- $(ls "$REF"/v1_ex_real_mode/*.MOO.gz | grep -v '/F4\.MOO')
fi
"$O/x86test" -k "$T/win16/x86known.txt" "$REF/80386.csv" "$@" > "$O/x86suite.log"
awk '$4 > 0' "$O/x86suite.log"
tail -1 "$O/x86suite.log"
