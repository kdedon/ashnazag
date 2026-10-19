#!/bin/sh
# host.sh -- startwin built with this host's cc (in-memory screen).
#
#   sh tests/win16/host.sh outdir
#
# Out: outdir/startwin.  FreeType's sources are fetched into toolchain/
# the first time (kernel/guest/win16/ft/freetype.sh).
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
G=$(cd "$T/.." && pwd)/kernel/guest/win16
B=$1
mkdir -p "$B/obj" "$B/ftobj"
FTF=$(sh "$G/ft/freetype.sh" flags)
for f in $(sh "$G/ft/freetype.sh" srcs); do
	o=$B/ftobj/$(basename "$f" .c).o
	[ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt "$G/ft/w16ftopt.h" ] && continue
	cc -O1 -w $FTF -c -o "$o" "$f"
done
for f in "$G"/*.c; do
	case $(basename "$f") in scr_fb.c|snd_so.c) continue ;; esac
	o=$B/obj/$(basename "$f" .c).o
	cc -std=gnu89 -O1 -w -I"$G" $FTF -c -o "$o" "$f"
done
cc -o "$B/startwin" "$B"/obj/*.o "$B"/ftobj/*.o -lm
