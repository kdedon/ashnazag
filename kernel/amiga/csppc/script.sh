#!/bin/sh
# script.sh outdir: assemble csppc.ss into outdir/csppc_script.h with the
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
"$O/ncr53cxxx" "$C/csppc.ss" -o "$O/csppc.out"
{ echo '#define u_int32_t uint'; sed -n -e '/^const/,/^};/p' -e '/^#define/p' "$O/csppc.out"; } > "$O/csppc_script.h"
