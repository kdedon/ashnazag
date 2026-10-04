#!/bin/sh
# cc.sh -- XView's cc: compiles with $XVTC's gcc (integer results copied
# into a0 too), links with amixld.sh.
set -e
unset AMIX_SYSROOT AMIX_CRT_DIR
D=$(cd "$(dirname "$0")" && pwd)
case " $* " in
*" -c "*|*" -E "*|*" -M "*) exec "${XVTC:?}/bin/m68k-cbm-sysv4-gcc" "$@" ;;
*) exec sh "$D/../config/amixld.sh" "$@" ;;
esac
