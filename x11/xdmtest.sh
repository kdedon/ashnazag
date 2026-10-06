#!/bin/sh
# xdmtest.sh -- boot an image made with BOOTX=1 in QEMU q800 and drive
# xdm and the session chooser by keyboard and mouse, with screen dumps.
#
#   sh x11/xdmtest.sh OUTDIR [image]
#
# guest logs in six times: the default session (twm, after the
# chooser's countdown), XView (2), twm (1), the Console terminal (6), and
# the TOS (4) and Mac (3) environments, which Control-C on the console
# ends.  Each session's end must bring back the xdm login.  The image
# needs XView and the Amiga environment for these numbers.
AUX=$(cd "$(dirname "$0")/.." && pwd)
PLATFORM=${PLATFORM:-mac}
X11W=${X11W:-$AUX/images/work/x11-$PLATFORM}
OUT=$1
IMG=${2:-$X11W/q800-mac.img}
# N relative moves of DX DY: ADB carries 7-bit deltas, and the server
# accelerates steps over 4 pixels
mv() {
	j=0
	while [ $j -lt $1 ]; do steps="$steps 'hmp:mouse_move $2 $3'"; j=$((j + 1)); done
}
# twm's root menu at the bottom right corner, where the pointer is on its
# last entry, Exit
steps=; mv 40 16 16; EXIT="$steps wait:2 'hmp:mouse_button 1' wait:3"
# pointer to the top left, then into the terminal (16, 40 to 508, 380)
# whatever the acceleration
steps=; mv 40 -16 -16; mv 6 16 16; HOME200=$steps

set -- wait:150 shot:01-xdm 'type:guest\n' wait:2 shot:02-password 'type:\n' \
	wait:12 shot:03-chooser wait:40 shot:04-twm
eval "set -- \"\$@\" $EXIT"
set -- "$@" shot:05-twm-menu 'hmp:mouse_button 0' wait:20 shot:07-xdm-again
# XView; click in the terminal to focus it, end olvwm from there
set -- "$@" 'type:guest\n' wait:2 'type:\n' wait:7 shot:08-chooser-default 'type:2' wait:60
eval "set -- \"\$@\" $HOME200"
set -- "$@" 'hmp:mouse_button 1' wait:1 'hmp:mouse_button 0' wait:2 'hmp:mouse_button 1' wait:1 \
	'hmp:mouse_button 0' wait:2 shot:09-xview \
	'type:ps -e | grep olvwm | while read p r; do kill $p; done\n' wait:20 shot:10-xdm-again
# twm by its number: the session log, the server's options
set -- "$@" 'type:guest\n' wait:2 'type:\n' wait:7 'type:1' wait:40 \
	'type:tail -8 .xsession-log; ps -ef | grep Xami\n' wait:8 shot:11-twm-log
eval "set -- \"\$@\" $EXIT"
set -- "$@" 'hmp:mouse_button 0' wait:20
# Console: a full-screen terminal; exit ends it
set -- "$@" 'type:guest\n' wait:2 'type:\n' wait:7 'type:6' wait:30 \
	'type:cat .xsession-choice; ls -l /usr/x11r6/lib/X11/xdm\n' wait:8 shot:12-console \
	'type:exit\n' wait:20 shot:13-xdm-again
# TOS from the chooser; Control-C on the console ends it, then xdm
set -- "$@" 'type:guest\n' wait:2 'type:\n' wait:7 'type:4' wait:90 shot:14-tos \
	'hmp:sendkey ctrl-alt-meta_l-0' wait:3 shot:15-console 'hmp:sendkey ctrl-c' wait:25 \
	shot:16-xdm-after-tos
# the Mac from the chooser, ended the same way
set -- "$@" 'type:guest\n' wait:2 'type:\n' wait:7 'type:3' wait:150 shot:17-mac \
	'hmp:sendkey ctrl-alt-meta_l-0' wait:3 shot:18-console 'hmp:sendkey ctrl-c' wait:30 \
	shot:19-xdm-after-mac quit
exec sh "$AUX/images/qemu/run-mac.sh" --tmo 2000 "$AUX/Quadra 800.ROM" "$IMG" 128 "$OUT" "$@"
