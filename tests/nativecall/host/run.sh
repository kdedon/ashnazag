#!/bin/sh
set -eu
T=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
AUX=$(CDPATH= cd -- "$T/.." && pwd)
B=$(mktemp -d /tmp/nativecall-host.XXXXXX)
trap 'rm -rf "$B"' EXIT HUP INT TERM
nice -n 19 "${HOSTCC:-cc}" -std=gnu89 -Wall -Wextra -Werror \
    -I"$T/nativecall/host" -I"$AUX/kernel/guest/include" \
    "$AUX/kernel/guest/mod/guestcore/gnative.c" "$T/nativecall/host/test.c" -o "$B/test"
"$B/test"
python3 "$T/nativecall/host/signals.py"
