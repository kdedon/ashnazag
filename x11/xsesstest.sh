#!/bin/sh
# xsesstest.sh -- console boot (no BOOTX): guest runs `xsession twm`,
# leaves by twm's Exit, then `xsession tos` and Control-C, back at the
# shell each time.  Screen dumps in OUTDIR.
#
#   sh x11/xsesstest.sh OUTDIR [image]
AUX=$(cd "$(dirname "$0")/.." && pwd)
X11W=${X11W:-$AUX/images/work/x11}
OUT=$1
IMG=${2:-$X11W/q800-mac.img}
set -- wait:120 'serial:guest\n' wait:10 'serial:xsession twm\n' wait:60 shot:01-twm
# the root menu at the bottom right corner: the pointer is on Exit
j=0
while [ $j -lt 40 ]; do set -- "$@" 'hmp:mouse_move 16 16'; j=$((j + 1)); done
set -- "$@" wait:2 'hmp:mouse_button 1' wait:3 shot:02-menu 'hmp:mouse_button 0' wait:15 \
	'serial:echo back; xsession tos\n' wait:90 shot:03-tos 'hmp:sendkey ctrl-alt-meta_l-0' wait:3 'hmp:sendkey ctrl-c' wait:20 \
	'serial:echo again\n' wait:5 shot:04-shell quit
exec sh "$AUX/images/qemu/run-mac.sh" --sock --tmo 600 "$AUX/Quadra 800.ROM" "$IMG" 128 "$OUT" "$@"
