#!/bin/sh
# Fails on a plain "volatile" in the sources built with -traditional, where
# sys/types.h defines it away; write __volatile__.
K=$(cd "$(dirname "$0")/.." && pwd)
cd "$K" || exit 1
bad=$(find amiga atari dlm drm mac otbridge guest/mod guest/include guest/voyage200 guest/guest_shim.c \
	\( -path amiga/test -o -path '*/review' \) -prune -o \
	\( -name '*.c' -o -name '*.h' \) -print |
	xargs grep -nw volatile |
	grep -v '^[^:]*:[0-9]*:[ 	]*\(/\*\|\*[ 	]\|\*$\|//\)')
if [ -n "$bad" ]; then
	echo "$bad"
	echo "[FAIL] plain volatile in -traditional code: use __volatile__"
	exit 1
fi
