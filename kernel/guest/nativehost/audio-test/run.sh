#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
K=$(CDPATH= cd -- "$D/../../.." && pwd)
OUT=$(mktemp -d /tmp/native-audio-test.XXXXXX)
trap 'rm -rf "$OUT"' EXIT HUP INT TERM
mkdir -p "$OUT/sys"
printf '#define I_NREAD 0x5301\n' > "$OUT/sys/stropts.h"
nice -n 19 cc -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -Wno-unused-parameter -I"$OUT" -I"$D/.." -I"$K/mac/sound" \
    "$D/../nativehost_audio.c" "$D/test.c" -Wl,--wrap=open -Wl,--wrap=ioctl \
    -Wl,--wrap=write -Wl,--wrap=read -Wl,--wrap=close -o "$OUT/test"
"$OUT/test"
