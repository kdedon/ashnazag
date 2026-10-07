#!/bin/sh
# mods.sh kernel.elf outdir -- the sound drivers as modules for that
# kernel, from build.sh's objects in outdir: outdir/mod.d/{asc,auxsnd}
# and outdir/modadmin, which registers them at boot (majors 46, 47).
set -e
D=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$D/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
DLM=$K/dlm
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
KERNEL=$1 O=$2
HCC="nice -n 19 ${HOSTCC:-cc} -std=gnu89 -O -w"
CC="nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -Wall -Wno-comment -I$DLM/include"

mkdir -p "$O/tools" "$O/src"
$HCC -DDLM_TOOL -I"$DLM" -I"$DLM/include" -o "$O/tools/mkksym" "$DLM/tools/mkksym.c" "$DLM/dlm_sym.c"
$HCC -o "$O/tools/modfix" "$DLM/tools/modfix.c"
"$O/tools/mkksym" -x "$KERNEL" > "$O/exports"
rm -rf "$O/mod.d"
for m in asc:snd:ascmod auxsnd:auxsnd:sndmod; do
	n=${m%%:*} r=${m#*:}
	d=$O/src/$n
	rm -rf "$d"; mkdir -p "$d"
	$CC -c "$D/${r#*:}.c" -o "$d/wrap.o" 2> "$d/wrap.warn" || { cat "$d/wrap.warn"; exit 1; }
	if grep -v 'types.h:182\|In file included\|^ *from ' "$d/wrap.warn" | grep .; then
		echo "[FAIL] warnings in ${r#*:}.c"; exit 1
	fi
	m68k-cbm-sysv4-ld -r -o "$d/Driver.o" "$O/${r%:*}.o" "$d/wrap.o"
	cp "$D/mod/$n/Master" "$d/"
	DLM_TOOLS=$O/tools sh "$DLM/tools/mkmod" -e "$O/exports" -o "$O" "$d"
done
sh "$DLM/tools/modadmin.sh" "$O/src"
cp "$O/src/modadmin" "$O/modadmin"
