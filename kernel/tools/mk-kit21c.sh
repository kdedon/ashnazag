#!/bin/sh
# mk-kit21c.sh -- 2.1p2a (2.1c) link kit with the patch disk's kernel layout.
#
#   sh tools/mk-kit21c.sh          -> kit-2.1c/usr/sys
#
# The patch disk ships source only (c0.c, aen.c, kernel.c) and relinks with
# the native AMIX cc, which this host cannot run.  This reproduces the
# resulting object sizes from the shipped objects:
#   c0.o      + console_depth (4 bytes .data)
#   aen/exp   + 44 bytes .text (new oversize-packet check; code not rebuilt)
#   kernel.o  bbpoll in cdevsw[4], no static uname, "SYS/TS/RT" in .text
# so every other object lands where the patched kernel has it.  c0 and aen
# keep their 2.1 behaviour.
set -e

K=/home/kevin/git/aux/kernel
AUX=/home/kevin/git/aux
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
SRC="$AUX/toolchain/amix-root/usr/sys"
PD="$K/patch/payload/root/usr/sys"
OUT="$K/kit-2.1c/usr/sys"
W="$K/kit-2.1c/work"

[ ! -d "$K/kit-2.1c" ] || rm -r "$K/kit-2.1c"
mkdir -p "$OUT" "$W"
cp -a "$SRC/." "$OUT/"
for f in amiga/console/c0.c amiga/driver/aen/aen.c master.d/kernel.c; do
	cp -p "$PD/$f" "$OUT/$f"
done

cat > "$W/c0pad.s" <<'EOS'
	.data
	.globl	console_depth
console_depth:
	.long	1
EOS
cat > "$W/aenpad.s" <<'EOS'
	.text
	.space	44,0x4e
EOS
m68k-cbm-sysv4-as -o "$W/c0pad.o" "$W/c0pad.s"
m68k-cbm-sysv4-as -o "$W/aenpad.o" "$W/aenpad.s"
m68k-cbm-sysv4-ld -r -o "$OUT/amiga/console/c0.o" "$SRC/amiga/console/c0.o" "$W/c0pad.o"
m68k-cbm-sysv4-ld -r -o "$OUT/amiga/driver/aen/exp" "$SRC/amiga/driver/aen/exp" "$W/aenpad.o"
rm -f "$OUT/amiga/driver/aen/aen.o"

python3 "$K/tools/obj2s.py" "$SRC/master.d/kernel.o" "$W/kernel.s" --drop-text \
	--drop-sym uname --data-to-text 0x1930:0x193c --reloc 0xbb8=bbpoll
m68k-cbm-sysv4-as -o "$OUT/master.d/kernel.o" "$W/kernel.s"
m68k-cbm-sysv4-size "$OUT/amiga/console/c0.o" "$OUT/amiga/driver/aen/exp" "$OUT/master.d/kernel.o"
echo "[OK] $OUT"
