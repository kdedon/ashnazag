#!/bin/sh
set -eu
T=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=$(CDPATH= cd -- "$T/../.." && pwd)
B=$(mktemp -d /tmp/voyage200-host.XXXXXX)
trap 'rm -rf "$B"' EXIT HUP INT TERM
nice -n 19 "${HOSTCC:-cc}" -std=gnu89 -Wall -Wextra -Werror \
	-I"$A/kernel/guest/voyage200" "$A/kernel/guest/voyage200/hash.c" \
	"$T/hash-check.c" -o "$B/hash-check"
"$B/hash-check"
