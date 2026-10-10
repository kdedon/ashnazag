#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$D/../../.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
AMIX=${AMIX_ROOT:-/home/kevin/git/aux/toolchain/amix-root}
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -Wextra -Werror \
    -fsanitize=address,undefined -o "$W/provider" \
    "$D/record.c" "$D/message.c" "$D/provider.c" "$D/bootstrap.c" "$D/test/provider.c"
ASAN_OPTIONS=detect_leaks=0 nice -n19 "$W/provider"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -Wextra -Werror -Wno-comment \
    -I"$AMIX/usr/include" -fsanitize=address,undefined -o "$W/host-adapter" \
    "$D/host.c" "$D/bootstrap.c" "$D/test/host-adapter.c"
ASAN_OPTIONS=detect_leaks=0 nice -n19 "$W/host-adapter"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -Wextra -Werror -Wno-comment \
    -I"$AMIX/usr/include" -fsanitize=address,undefined -o "$W/host-read-retry" \
    "$D/host.c" "$D/record.c" "$D/message.c" "$D/provider.c" "$D/test/host-read-retry.c"
ASAN_OPTIONS=detect_leaks=0 nice -n19 "$W/host-read-retry"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
for source in provider bootstrap host; do
    nice -n19 m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 -Wall -Werror -Wno-comment \
        -c "$D/$source.c" -o "$W/$source.o"
done
"$AUX/toolchain/linux/bin/m68k-linux-gnu-as" -m68020 -o "$W/hostio.o" "$D/hostio.s"
echo 'PASS AMIX 68020 provider/host builds and ABI guards'
