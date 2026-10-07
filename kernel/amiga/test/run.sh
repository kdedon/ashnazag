#!/bin/sh
# run.sh -- build the amilib/opci harness for an emulated 68040 and run it.
#
#   sh kernel/amiga/test/run.sh [openpci.library]
#
# Needs m68k-linux-gnu-gcc (any version; the code is K&R, as for the AMIX
# compiler) and python3 with unicorn (pip install unicorn).  The library
# defaults to /etc/conf/pci/openpci.library's place in the tree,
# $OPENPCI or media/openpci.library; without it the openpci tests are
# skipped.  One PASS/FAIL line per check.
set -e

T=$(cd "$(dirname "$0")" && pwd)
A=$(cd "$T/.." && pwd)
AUX=$(cd "$A/../.." && pwd)
B=$A/build/test
mkdir -p "$B"
LIB=${1:-${OPENPCI:-$AUX/media/openpci.library}}
[ -f "$LIB" ] || LIB=

CC="m68k-linux-gnu-gcc -m68060 -O2 -ffreestanding -fno-builtin -fno-pie \
 -fno-tree-loop-distribute-patterns -Wall -Wno-parentheses -Wno-old-style-definition \
 -I$A/amilib -I$A/include"
objs=
for c in "$A"/amilib/amexec.c "$A"/amilib/amlibs.c "$A"/amilib/amhunk.c \
    "$A"/opci/opci.c "$T"/hplat.c "$T"/hmain.c; do
	o=$B/$(basename "$c" .c).o
	$CC -c "$c" -o "$o"
	objs="$objs $o"
done
m68k-linux-gnu-as -m68040 "$A/amilib/amglue.s" -o "$B/amglue.o"
m68k-linux-gnu-as -m68040 "$T/hstart.s" -o "$B/hstart.o"
m68k-linux-gnu-ld -T "$T/h.ld" -o "$B/h.elf" "$B/hstart.o" $objs "$B/amglue.o" \
	$(m68k-linux-gnu-gcc -m68040 -print-libgcc-file-name)
m68k-linux-gnu-objcopy -O binary "$B/h.elf" "$B/h.bin"
python3 "$T/run.py" "$B/h.elf" "$B/h.bin" $LIB
