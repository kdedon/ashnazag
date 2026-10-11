#!/bin/sh
set -e
A=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$A/.." && pwd)
AUX=$(cd "$K/.." && pwd)
PORT=$K/amix-040-060-port
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
BASE=${1:-$PORT/build/unix-040}
OUT=${2:-$K/build/unix-amiga}
W=$K/build/amiga
mkdir -p "$W"
[ -f "$BASE" ] || { echo "[FAIL] no base $BASE"; exit 1; }
sh "$K/dlm/build.sh" -k "$W/dlm"
sh "$K/guest/build.sh" -k "$BASE" "$W/guest"
nice -n19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68000 -DDS_AMIGA -Wall -Wno-comment -fno-common \
 -I"$K/mac/display" -I"$K/mac/video" -I"$K/mac/adb" -c "$K/mac/display/ds.c" -o "$W/ds.o"
nice -n19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68000 -DDS_AMIGA -Wall -Wno-comment -fno-common \
 -I"$K/mac/display" -I"$K/mac/video" -I"$K/mac/adb" -I"$A/include" -c "$A/amigads.c" -o "$W/amigads.o"
WEAK="--weaken-symbol dmainit --weaken-symbol execsw --weaken-symbol fmodsw"
OVR="ev_config ev_fork ev_exec ev_exit sendsig valid_usr_range fsig kbintr unt_latch vtop hat_ptalloc uiomove aciabintr"
for s in $OVR; do WEAK="$WEAK --weaken-symbol $s"; done
for s in sendsig valid_usr_range fsig kbintr unt_latch hat_ptalloc uiomove aciabintr; do
 v=$(m68k-linux-gnu-nm "$BASE" | awk -v s="$s" '$3==s && $2=="T"{print $1}')
 [ -n "$v" ] || { echo "[FAIL] no $s"; exit 1; }
 WEAK="$WEAK --add-symbol __amix_$s=.text:0x$v,function,global"
done
for pair in execsw:data fmodsw:data; do
 s=${pair%:*}; sec=${pair#*:}
 v=$(m68k-linux-gnu-nm "$BASE" | awk -v s="$s" '$3==s && $2=="D"{print $1}')
 [ -n "$v" ] || { echo "[FAIL] no $s"; exit 1; }
 WEAK="$WEAK --add-symbol __amix_$s=.$sec:0x$v,object,global"
done
m68k-linux-gnu-objcopy $WEAK --globalize-symbol freemem_wait "$BASE" "$W/base.weak"
python3 "$K/mac/patch_s5pages.py" "$W/base.weak"
python3 "$K/mac/vtop/patch_vtop.py" "$W/base.weak"
python3 "$A/patch_tt.py" "$W/base.weak"
for f in vtop/vtop ptalloc/ptalloc; do
 nice -n19 m68k-cbm-sysv4-gcc -m68040 -c "$K/mac/$f.s" -o "$W/$(basename "$f").o"
done
nice -n19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -c "$K/mac/s5dir/uiomod.c" -o "$W/uiomod.o"
nice -n19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -I"$K/dlm/include" -c "$A/amigadlm.c" -o "$W/amigadlm.o"
nice -n19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -c "$A/amigarom.c" -o "$W/amigarom.o"
nice -n19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -I"$A/amilib" -c "$A/amigazorro.c" -o "$W/amigazorro.o"
nice -n19 m68k-cbm-sysv4-gcc -m68040 -c "$PORT/src/devkvmap040.s" -o "$W/devkvmap.o"
OBJS="$W/vtop.o $W/ptalloc.o $W/uiomod.o $W/amigadlm.o $W/amigarom.o $W/amigazorro.o $W/devkvmap.o $W/ds.o $W/amigads.o $W/guest/guest.o"
for f in dlm_core dlm_ld dlm_sym dlm_slot dlm_str dlmconf dlm_cache dlm_hooksw; do
 OBJS="$OBJS $W/dlm/$f.o"
done
nice -n19 m68k-cbm-sysv4-gcc -m68040 -c "$K/atari/ata060math.s" -o "$W/math.o"
OBJS="$OBJS $W/math.o"
python3 "$K/atari/chk64.py" "$W/ds.o" "$W/amigads.o" "$W/math.o"
m68k-cbm-sysv4-ld -r -o "$W/pass1" "$W/base.weak" $OBJS "$W/dlm/dlmksym.o"
python3 "$K/guest/patch_vec.py" "$W/pass1" "$W/guest/gates.lst"
m68k-elf-ld -T "$A/amiga.ld" -o "$W/pass1.elf" "$W/pass1"
"$W/dlm/mkksym" -o "$W/symbols.bin" "$W/pass1.elf"
python3 "$A/relksym.py" "$W/symbols.bin" > "$W/symbols.s"
m68k-cbm-sysv4-gcc -m68040 -c "$W/symbols.s" -o "$W/symbols.o"
m68k-cbm-sysv4-ld -r -o "$OUT" "$W/base.weak" $OBJS "$W/symbols.o"
python3 "$K/guest/patch_vec.py" "$OUT" "$W/guest/gates.lst"
m68k-elf-ld -T "$A/amiga.ld" -o "$OUT.elf" "$OUT"
"$W/dlm/mkksym" -c "$OUT.elf"
python3 "$PORT/src/check_relink_relocs.py" "$OUT" > "$W/validator.log"
tail -1 "$W/validator.log"
python3 "$K/dlm/tools/chkdlm.py" "$OUT.elf"
python3 "$K/mac/patch_s5pages.py" -c "$OUT.elf"
python3 "$K/mac/vtop/patch_vtop.py" -c "$OUT.elf"
python3 "$A/patch_tt.py" -c "$OUT.elf"
python3 "$A/test/relksym.py" "$OUT"
echo "[OK] $OUT is loader-relocatable; $OUT.elf is the zero-origin symbol view"
python3 "$K/guest/chkguest.py" "$OUT.elf" "$W/guest/gates.lst"
sh "$K/guest/build.sh" -m "$OUT.elf" "$W/guestmodules"
