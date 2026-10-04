#!/bin/sh
# runtest.sh -- boot the X image in QEMU q800, start X, exercise it, dump screens.
#
#   sh x11/runtest.sh OUTDIR [image]
#
# Login and startx go over the serial console; everything after that uses
# the ADB keyboard and mouse only.
AUX=$(cd "$(dirname "$0")/.." && pwd)
X11W=${X11W:-$AUX/images/work/x11}
OUT=$1
IMG=${2:-$X11W/q800-x11.img}
set -- wait:90 shot:boot 'serial:root\n' wait:8 'serial:/usr/x11r6/bin/startx &\n' \
	wait:90 shot:x1 \
	'type:xdpyinfo | head -24\n' wait:15 shot:x2 \
	'type:clear; cat /tmp/Xamix.log\n' wait:5 shot:x3 \
	'hmp:sendkey ctrl-alt-meta_l-0' wait:5 shot:console \
	'hmp:sendkey ctrl-alt-meta_l-1' wait:5 shot:xback
set -- "$@" 'type:xargs kill -9 < /tmp/Xamix.pid\n' wait:10 shot:killed \
	'type:/usr/x11r6/bin/startx &\n' wait:60 shot:again
i=0
while [ $i -lt 13 ]; do set -- "$@" 'hmp:mouse_move 10 10'; i=$((i + 1)); done
set -- "$@" 'hmp:mouse_move 10 0' 'hmp:mouse_move 10 0' wait:2 \
	'hmp:mouse_button 1' wait:3 shot:menu 'hmp:mouse_button 0' wait:10 shot:twmexit quit
# DEPTH: boot depth for QEMU's DAFB (-g 640x480xDEPTH)
[ -n "$DEPTH" ] && QEXTRA="$QEXTRA -g 640x480x$DEPTH" && export QEXTRA
exec sh "$AUX/images/qemu/run-mac.sh" --sock --tmo 480 "$AUX/Quadra 800.ROM" "$IMG" 32 "$OUT" "$@"
