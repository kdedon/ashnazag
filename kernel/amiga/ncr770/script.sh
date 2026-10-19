#!/bin/sh
# script.sh outdir: assemble ncr770.ss into outdir/ncr770_script.h with the
# SCRIPTS assembler from the NetBSD sources setup.sh fetched.
set -e
C=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$C/../../.." && pwd)
O=$1
mkdir -p "$O"
[ -x "$O/ncr53cxxx" ] || {
	tar -xzf "$AUX/toolchain/dl/syssrc.tgz" -C "$O" usr/src/sys/dev/microcode/siop/ncr53cxxx.c
	${HOSTCC:-cc} -O -w -D'__RCSID(x)=' -o "$O/ncr53cxxx" "$O/usr/src/sys/dev/microcode/siop/ncr53cxxx.c"
}
"$O/ncr53cxxx" "$C/ncr770.ss" -o "$O/ncr770.out"
{ echo '#define u_int32_t uint'; sed -n -e '/^const/,/^};/p' -e '/^#define/p' "$O/ncr770.out"; } > "$O/ncr770_script.h"
