#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
B=$(mktemp -d /tmp/nativehost-test.XXXXXX)
trap 'rm -rf "$B"' EXIT HUP INT TERM
nice -n 19 "${HOSTCC:-cc}" -std=gnu89 -Wall -Wextra -Werror \
    -I"$D" -I"$D/../../mac/display" "$D/nativehost.c" "$D/test/test.c" \
    -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=mmap,--wrap=munmap \
    -Wl,--wrap=poll,--wrap=read,--wrap=write,--wrap=lseek -o "$B/test"
"$B/test"
