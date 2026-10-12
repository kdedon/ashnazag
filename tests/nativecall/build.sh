#!/bin/sh
set -eu
T=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
AUX=$(CDPATH= cd -- "$T/.." && pwd)
B=${1:-$T/build/nativecall}
sh "$T/nativehost/build.sh" "$B"
B=$(CDPATH= cd -- "$B" && pwd)
TC=${AMIX_TC:-$AUX/toolchain/amix}
KERNEL=${KERNEL:-$AUX/kernel/build/unix-mac.elf}
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
CC=$TC/bin/m68k-cbm-sysv4-gcc
AS=$TC/bin/m68k-cbm-sysv4-as
LD=$TC/bin/m68k-cbm-sysv4-ld
G=$AUX/kernel/guest
DLM=$AUX/kernel/dlm
mkdir -p "$B/core"
objects=
for source in "$G/mod/guestcore"/*.c; do
    object=$B/core/$(basename "$source" .c).o
    nice -n 19 "$CC" -traditional -D_KERNEL -DSYSV -Dinline=__inline__ \
        -Dvolatile=__volatile__ -m68040 -O -fno-builtin -fno-common -Wall -Wno-comment \
        -I"$G/include" -I"$DLM/include" -c "$source" -o "$object"
    objects="$objects $object"
done
nice -n 19 "$AS" -o "$B/core/gates.o" "$G/mod/guestcore/gates.s"
cat > "$B/core/moddep.s" <<'ASM'
    .section .moddata,"aw",@progbits
    .balign 4
    .long guestcore_wrapper
    .byte 0
ASM
nice -n 19 "$AS" -o "$B/core/moddep.o" "$B/core/moddep.s"
nice -n 19 "$LD" -r -o "$B/guestcore" $objects "$B/core/gates.o" "$B/core/moddep.o"
"$B/tools/modfix" -e "$B/exports" -p guestcore -m guestcore "$B/guestcore"
ln -sf "${M68K_OBJDUMP:-$TC/../bin/m68k-elf-objdump}" "$B/tools/m68k-elf-objdump"
PATH="$B/tools:$PATH" python3 "$G/chkgatemod.py" "$B/guestcore"
nice -n 19 "$CC" -traditional -D_KERNEL -DSYSV -Dinline=__inline__ \
    -Dvolatile=__volatile__ -m68040 -O -fno-builtin -fno-common -Wall -Wno-comment \
    -I"$G/include" -I"$DLM/include" -I"$T/nativecall" \
    -c "$T/nativecall/profile.c" -o "$B/obj/nhcall.o"
cat > "$B/obj/callmoddep.s" <<'ASM'
    .section .moddata,"aw",@progbits
    .balign 4
    .long nhcall_wrapper
    .ascii "guestcore"
    .byte 0
ASM
nice -n 19 "$AS" -o "$B/obj/callmoddep.o" "$B/obj/callmoddep.s"
nice -n 19 "$LD" -r -o "$B/nhcall" "$B/obj/nhcall.o" "$B/obj/callmoddep.o"
"$B/tools/modfix" -e "$B/exports" -d "$B/guestcore" -p nhcall -m nhcall "$B/nhcall"
nice -n 19 "$CC" -O -Wall -Wno-comment -D__STDC__=0 \
    -I"$G/include" -I"$G/nativehost" -I"$AUX/kernel/mac/display" \
    -I"$DLM/include" -I"$T/src" -I"$T/nativecall" \
    -c "$T/src/t_nativecall.c" -o "$B/obj/t_nativecall.o"
nice -n 19 "$AS" -o "$B/obj/nativecall.o" "$G/nativehost/nativecall.s"
nice -n 19 "$AS" -o "$B/obj/probe.o" "$T/nativecall/probe.s"
nice -n 19 "$AS" -o "$B/obj/modadm.o" "$DLM/libmod/modadm.s"
for f in "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a; do LIBGCC=$f; done
nice -n 19 "$LD" -o "$B/t_nativecall" "$LIB/crt1.o" "$LIB/crti.o" \
    "$B/obj/t_nativecall.o" "$B/obj/nativecall.o" "$B/obj/probe.o" \
    "$B/obj/nativehost.o" "$B/obj/t.o" "$B/obj/modload.o" "$B/obj/moduload.o" "$B/obj/modadm.o" \
    "$SYS/usr/lib/libc.so.1" "$B/libextra.a" "$LIBGCC" "$LIB/crtn.o"
cat > "$B/inittab" <<'INIT'
is:s:initdefault:
ap::sysinit:/sbin/autopush -f /etc/ap/chan.ap
tt::sysinit:/tests/runall t_nativecall:35 </dev/console >/dev/console 2>&1
INIT
{
    echo 'c /dev/nhcall 600 0 3 63 0'
    for name in guestcore nhcall t_nativecall; do
        echo "f /tests/$name 755 0 3 $B/$name"
    done
} >> "$B/root.manifest"
nice -n 19 python3 "$AUX/kernel/mac/ramdisk/mks5fs.py" -s 4096 -i 256 -b 1024 \
    -t 723000000 "$B/root.manifest" "$B/nativecall.img"
python3 "$AUX/kernel/mac/ramdisk/s5check.py" "$B/nativecall.img" -l > "$B/nativecall.lst"
test "$(wc -c < "$B/nativecall.img")" -le 33554432
echo "READY $B/t_nativecall $B/nativecall.img"
