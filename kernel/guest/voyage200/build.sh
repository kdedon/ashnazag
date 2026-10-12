#!/bin/sh
set -eu
V=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=$(CDPATH= cd -- "$V/../../.." && pwd)
B=${1:-$A/kernel/guest/build/voyage200}
TC=${AMIX_TC:-$A/toolchain/amix}
KERNEL=${KERNEL:-$A/kernel/build/unix-mac.elf}
CORE=${GUESTCORE:-$A/kernel/guest/build/mod.d/guestcore}
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
CC=$TC/bin/m68k-cbm-sysv4-gcc
AS=$TC/bin/m68k-cbm-sysv4-as
LD=$TC/bin/m68k-cbm-sysv4-ld
D=$A/kernel/dlm
mkdir -p "$B/obj" "$B/extra" "$B/tools"
B=$(CDPATH= cd -- "$B" && pwd)
for name in voyage200 hash; do
	nice -n 19 "$CC" -O -Wall -Wno-comment -D__STDC__=0 -fno-builtin \
		-I"$V" -I"$A/kernel/guest/nativehost" -I"$A/kernel/mac/display" \
		-c "$V/$name.c" -o "$B/obj/$name.o"
done
nice -n 19 "$CC" -O -Wall -Wno-comment -D__STDC__=0 \
	-I"$A/kernel/guest/nativehost" -I"$A/kernel/mac/display" \
	-c "$A/kernel/guest/nativehost/nativehost.c" -o "$B/obj/nativehost.o"
nice -n 19 "$AS" -o "$B/obj/calls.o" "$V/calls.s"
(cd "$B/extra" && ar x "$LIB/libc.so" && rm -f libc.so.1 && ar rcs "$B/libextra.a" ./*.o)
for f in "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a; do LIBGCC=$f; done
nice -n 19 "$LD" -o "$B/native" "$LIB/crt1.o" "$LIB/crti.o" \
	"$B/obj/voyage200.o" "$B/obj/hash.o" "$B/obj/nativehost.o" "$B/obj/calls.o" \
	"$SYS/usr/lib/libc.so.1" "$B/libextra.a" "$LIBGCC" "$LIB/crtn.o"
nice -n 19 "$CC" -traditional -D_KERNEL -DSYSV -Dinline=__inline__ \
	-Dvolatile=__volatile__ -m68040 -O -fno-builtin -fno-common -Wall -Wno-comment \
	-I"$A/kernel/guest/include" -I"$D/include" -I"$V" \
	-c "$A/kernel/guest/mod/tiplatform/tiplatform.c" -o "$B/obj/tiplatform.o"
cat > "$B/obj/moddep.s" <<'ASM'
	.section .moddata,"aw",@progbits
	.balign 4
	.long tiplatform_wrapper
	.ascii "guestcore"
	.byte 0
ASM
nice -n 19 "$AS" -o "$B/obj/moddep.o" "$B/obj/moddep.s"
nice -n 19 "$LD" -r -o "$B/tiplatform" "$B/obj/tiplatform.o" "$B/obj/moddep.o"
nice -n 19 "${HOSTCC:-cc}" -std=gnu89 -O -w -DDLM_TOOL -I"$D" -I"$D/include" \
	-o "$B/tools/mkksym" "$D/tools/mkksym.c" "$D/dlm_sym.c"
nice -n 19 "${HOSTCC:-cc}" -std=gnu89 -O -w -o "$B/tools/modfix" "$D/tools/modfix.c"
"$B/tools/mkksym" -x "$KERNEL" > "$B/exports"
"$B/tools/modfix" -e "$B/exports" -d "$CORE" -p tiplatform -m tiplatform "$B/tiplatform"
mkdir -p "$B/mod.d" "$B/mod_register.d"
cp "$B/tiplatform" "$B/mod.d/tiplatform"
printf 'cdev\ttiplatform\t57\n' > "$B/mod_register.d/tiplatform"
if [ "$#" -gt 1 ]; then
	mkdir -p "$2/etc/conf/mod.d" "$2/usr/lib/voyage200"
	cp "$B/tiplatform" "$2/etc/conf/mod.d/tiplatform"
	cp "$B/native" "$2/usr/lib/voyage200/native"
	chmod 755 "$2/usr/lib/voyage200/native"
fi
echo "READY $B/native $B/tiplatform"
