#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
nice -n19 ${HOSTCC:-cc} -DOTR_DIAGNOSTICS -std=gnu89 -O -Wall -Wextra -Werror -Wno-misleading-indentation \
    -fsanitize=address,undefined -o "$W/mps" "$D/mps.c" "$D/provider.c" \
    "$D/bootstrap.c" "$D/record.c" "$D/message.c" "$D/test/mps.c"
ASAN_OPTIONS=detect_leaks=0 "$W/mps"
nice -n19 ${HOSTCC:-cc} -DOTR_DIAGNOSTICS -DOTSetupConfigurator=otr_setup_configurator \
    -O1 -Wall -Wextra -fsanitize=address,undefined -o "$W/configurator" \
    "$D/configurator.c" "$D/test/configurator.c"
ASAN_OPTIONS=detect_leaks=0 "$W/configurator"
sh "$D/build-provider.sh" "$W/normal"
PROVIDER_CFLAGS=-DOTR_DIAGNOSTICS sh "$D/build-provider.sh" "$W/diagnostic"
CONFIGURATOR_DIAGNOSTICS=1 sh "$D/build-configurator.sh" "$W/configurator-library"
python3 "$D/test/diagnostics.py" "$W/normal/mps.o" "$W/diagnostic/mps.o" \
    "$W/configurator-library/configurator-diag.o" "$W/configurator-library/configurator.o"
