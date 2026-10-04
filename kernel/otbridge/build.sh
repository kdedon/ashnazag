#!/bin/sh
# build.sh -- the otbridge module and its host tests.
#
#   sh kernel/otbridge/build.sh [-k kernel.elf] [outdir]
#
# Host: otxlate.c with -DOTB_HOST against test/otxtest.c, run; one
# PASS/FAIL line per check.  With -k: the module, checked by mkmod
# against the kernel's exports, into outdir/mod.d/otbridge.  A kernel
# without the STREAMS and driver linkages gets no module (exit 0).
set -e

O=$(cd "$(dirname "$0")" && pwd)
K=${KDIR:-$(cd "$O/.." && pwd)}
AUX=$(cd "$O/../.." && pwd)
KERNEL=
if [ "$1" = -k ]; then KERNEL=$2; shift 2; fi
OUT=${1:-$O/build}
mkdir -p "$OUT"
NICE="nice -n 19"

$NICE ${HOSTCC:-cc} -std=gnu89 -DOTB_HOST -O -Wall -Wno-parentheses -I"$O" \
	-o "$OUT/otxtest" "$O/test/otxtest.c" "$O/otxlate.c"
"$OUT/otxtest"

[ -n "$KERNEL" ] || exit 0
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/bin:$PATH"
export PATH
for s in mod_strops mod_drvops; do
	m68k-elf-nm "$KERNEL" | grep -q " D $s\$" || {
		echo "[skip] $KERNEL has no $s"; exit 0; }
done
DLM=$K/dlm
W=$OUT/work
mkdir -p "$W/tools" "$W/src"
$NICE ${HOSTCC:-cc} -std=gnu89 -O -w -DDLM_TOOL -I"$DLM" -I"$DLM/include" \
	-o "$W/tools/mkksym" "$DLM/tools/mkksym.c" "$DLM/dlm_sym.c"
$NICE ${HOSTCC:-cc} -std=gnu89 -O -w -o "$W/tools/modfix" "$DLM/tools/modfix.c"
CC="$NICE m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -Wall -I$DLM/include -I$O"
for f in otxlate otxti otbdev otbst; do
	$CC -c "$O/$f.c" -o "$W/$f.o" 2> "$W/$f.warn" || { cat "$W/$f.warn"; exit 1; }
	! grep -v 'types.h:182\|In file included\|^ *from ' "$W/$f.warn" | grep . || exit 1
done
cp "$O/Master" "$W/src/"
m68k-cbm-sysv4-ld -r -o "$W/src/Driver.o" "$W/otxlate.o" "$W/otxti.o" "$W/otbdev.o" "$W/otbst.o"
DLM_TOOLS=$W/tools sh "$DLM/tools/mkmod" -k "$KERNEL" -o "$OUT" "$W/src"
