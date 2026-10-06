#!/bin/sh
# relink-mac.sh -- Macintosh (Quadra 800, 68040) kernel from an AMIX base image.
#
#   sh mac/relink-mac.sh [base [out]]
#
# base: a relocatable AMIX kernel.  Default: the port's build/unix-040.
# out:  build/unix-mac (ld -r image) and build/unix-mac.elf (fully linked,
#       entry at mac_entry, for bootinfo loaders).  For A/UX
#       Startup, boot/mkcoff.sh wraps the ELF as build/unix-mac.coff.
#
# Drivers: SCC tty (console), 53C96 SCSI (sd.h layer), RAM disk (root
# image from ramdisk/build/root.img when present), SONIC Ethernet (DLPI,
# cdevsw[18]), display service (cdevsw[51-53]).  devtab/ replaces the
# device switches and picks the disk root.
# Same mechanism as the port's variants: weaken the Amiga platform entry
# points in the base, link the Mac objects over it, check that every
# override bound to our object, validate, then link at 0x10000.
set -e

MAC=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$MAC/.." && pwd)
AUX=$(cd "$K/.." && pwd)
PORT="$K/amix-040-060-port"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

BASE="${1:-$PORT/build/unix-040}"
OUT="${2:-$K/build/unix-mac}"
W="$K/build/mac"
mkdir -p "$W"
[ -f "$BASE" ] || { echo "[FAIL] no base image: $BASE"; exit 1; }

if m68k-linux-gnu-nm "$BASE" | grep -q ' [DdBb] kptr040$'; then
	MODE=port; ADAPT=baseport
else
	MODE=stock; ADAPT=basestock
fi
echo "[*] base $BASE ($MODE)"

echo "[*] assembling / compiling the Mac platform objects"
# BOOTDIAG=1: status lines at the bottom of the screen (bootdiag/diag.c)
DIAGAS=
AMIX_DIAG_CFLAGS=
if [ "${BOOTDIAG:-0}" = 1 ]; then
	DIAGAS=-Wa,--defsym,BOOTDIAG=1
	AMIX_DIAG_CFLAGS=-DBOOTDIAG
	echo "[*] BOOTDIAG: boot status lines"
fi
export AMIX_DIAG_CFLAGS
for s in macentry macintr pstartmac $ADAPT; do
	m68k-cbm-sysv4-gcc -m68040 $DIAGAS -c "$MAC/$s.s" -o "$W/$s.o"
done
m68k-cbm-sysv4-gcc -m68040 -c "$MAC/boot/auxentry.s" -o "$W/auxentry.o"
m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS $AMIX_DIAG_CFLAGS -m68040 -c "$MAC/macconf.c" -o "$W/macconf.o"
OBJS="$W/macentry.o $W/auxentry.o $W/macconf.o $W/macintr.o $W/pstartmac.o $W/$ADAPT.o"

echo "[*] drivers: SCC tty, 53C96 SCSI, RAM disk, frame-buffer console, ADB, SONIC, RTC, display, sound"
sh "$MAC/scc/build.sh" "$W/scc"
sh "$MAC/scsi/build.sh" "$W"
sh "$MAC/ramdisk/build.sh"
sh "$MAC/devtab/build.sh" "$W/devtab"
sh "$MAC/video/build.sh" "$W/video"
ADB_FBCONS=1 sh "$MAC/adb/build.sh" "$W/adb"	# keyboard -> fbcons_input
sh "$MAC/sonic/build.sh" "$W"
sh "$MAC/rtc/build.sh" "$W/rtc"
sh "$MAC/display/build.sh" "$W/display"
sh "$MAC/sound/build.sh" "$W/sound"
RDB="$MAC/ramdisk/build"
OBJS="$OBJS $W/scc/scc.o $W/scc/scccons.o $W/macscsi.o $RDB/rd.o $RDB/rdimage.o"
OBJS="$OBJS $W/devtab/devtab.o $W/adb/adb.o $W/macsonic.o $W/rtc/rtc.o"
OBJS="$OBJS $W/video/fbcons.o $W/video/fbprobe.o $W/video/fbfont.o $W/video/fbtty.o"
OBJS="$OBJS $W/display/ds.o $W/sound/snd.o $W/sound/auxsnd.o"

echo "[*] loadable modules: loader, system calls 64..69, kernel symbol table"
sh "$K/dlm/build.sh" -k "$W/dlm"
OBJS="$OBJS $W/dlm/dlm.o"

echo "[*] guest processes: shims, hook table, vector gates"
sh "$K/guest/build.sh" -k "$BASE" "$W/guest"
OBJS="$OBJS $W/guest/guest.o"

# 040 base: user addresses in vtop go through the process's page tables.
VTOP=
PTALLOC=
GLOB=
if [ $MODE = port ]; then
	echo "[*] vtop: user addresses through the process's page tables"
	m68k-cbm-sysv4-gcc -m68040 -c "$MAC/vtop/vtop.s" -o "$W/vtop.o"
	OBJS="$OBJS $W/vtop.o"
	VTOP=vtop
	echo "[*] hat_ptalloc: waiting table requests wait for free memory"
	m68k-cbm-sysv4-gcc -m68040 -c "$MAC/ptalloc/ptalloc.s" -o "$W/ptalloc.o"
	OBJS="$OBJS $W/ptalloc.o"
	PTALLOC=hat_ptalloc
fi

echo "[*] uiomove: kernel writes through segkmap mark the pages modified"
m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 -c "$MAC/s5dir/uiomod.c" -o "$W/uiomod.o"
OBJS="$OBJS $W/uiomod.o"

# Nothing in macconf.o may live in .bss: config() runs before BSS is cleared.
if m68k-linux-gnu-nm "$W/macconf.o" | grep -q ' [bBC] '; then
	echo "[FAIL] macconf.o has BSS/common symbols"; exit 1
fi

# Amiga platform entry points replaced for the Mac.
OVR="pstart config putchar getchar callrom sysdump haltsys rtnfirm hw_clkstart clkreld
p1int p2int p3int p4int p5int p6int parinit qlintr slpoll autocon delayus inituname
sdopen sdqueue sdhardwarename sdpartition sdvalid sdblkno sddevsize
ramopen ramclose ramstrategy ramprint ramsize
config_orig
dmainit
ev_config ev_fork ev_exec ev_exit sendsig valid_usr_range fsig
clkset stime mdboot $VTOP $PTALLOC"
# Amiga data replaced: the console streamtab named by cdevsw[0] and oncons(),
# the device switches and io_start[].
OVRD="coinfo cdevsw bdevsw io_start execsw fmodsw"
# Stock bodies kept under __amix_<name>: the wrappers and dlm_init call them.
ALIAS="sendsig:T valid_usr_range:T fsig:T execsw:D stime:T fmodsw:D"
[ -n "$PTALLOC" ] && ALIAS="$ALIAS hat_ptalloc:T" GLOB=freemem_wait

echo "[*] FPU probe: null or idle frame, boot option nofpu"
m68k-cbm-sysv4-gcc -m68040 -c "$MAC/fpu/chkfpu.s" -o "$W/chkfpu.o"
OBJS="$OBJS $W/chkfpu.o"
OVR="$OVR chk_fpu"

# The fatal user-fault notice goes through the guest shim's filter.
if m68k-linux-gnu-nm "$BASE" | grep -q ' T unt_latch$'; then
	OVR="$OVR unt_latch"
	ALIAS="$ALIAS unt_latch:T"
fi

# uiomove: the wrapper marks the segkmap pages it writes modified.
OVR="$OVR uiomove"
ALIAS="$ALIAS uiomove:T"
if [ -n "$AMIX_DIAG_CFLAGS" ]; then
	m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS $AMIX_DIAG_CFLAGS -m68040 -c "$MAC/bootdiag/diag.c" \
		-o "$W/diag.o"
	m68k-cbm-sysv4-gcc -m68040 -c "$MAC/bootdiag/diagvec.s" -o "$W/diagvec.o"
	OBJS="$OBJS $W/diag.o $W/diagvec.o"
	OVR="$OVR vfs_mountroot swapconf exece fpuinit idle"
	ALIAS="$ALIAS vfs_mountroot:T swapconf:T exece:T fpuinit:T"
fi

echo "[*] weakening the Amiga platform entry points"
WEAK=""
for s in $OVR; do
	m68k-linux-gnu-nm "$BASE" | grep -q " T $s\$" || {
		echo "[FAIL] $s is not a global function in the base"; exit 1; }
	WEAK="$WEAK --weaken-symbol $s"
done
for s in $OVRD; do
	m68k-linux-gnu-nm "$BASE" | grep -q " D $s\$" || {
		echo "[FAIL] $s is not global data in the base"; exit 1; }
	WEAK="$WEAK --weaken-symbol $s"
done
for s in $GLOB; do
	WEAK="$WEAK --globalize-symbol $s"
done
for a in $ALIAS; do
	s=${a%:*} t=${a#*:}
	off=$(m68k-linux-gnu-nm "$BASE" | awk -v s="$s" -v t="$t" '$3==s && $2==t{print $1}')
	[ -n "$off" ] || { echo "[FAIL] no $s to alias"; exit 1; }
	if [ "$t" = T ]; then sec=.text kind=function; else sec=.data kind=object; fi
	WEAK="$WEAK --add-symbol __amix_$s=$sec:0x$off,$kind,global"
done
m68k-linux-gnu-objcopy $WEAK "$BASE" "$W/base.weak"
python3 "$MAC/patch_s5pages.py" "$W/base.weak"
[ $MODE = stock ] || python3 "$MAC/vtop/patch_vtop.py" "$W/base.weak"
[ $MODE = stock ] || python3 "$MAC/patch_kvmpages.py" "$W/base.weak"

# The switches must keep the base sizes: shadowcsw/shadowbsw and the
# counts come from the base.
for t in c b; do
	want=$(m68k-linux-gnu-nm "$BASE" | awk -v s=shadow${t}sw '$3==s{print $1}')
	have=$(m68k-linux-gnu-nm -S "$W/devtab/devtab.o" | awk -v s=${t}devsw '$4==s{print $2}')
	[ -n "$want" ] && [ $((0x$want)) -eq $((0x${have:-0})) ] || {
		echo "[FAIL] ${t}devsw is 0x$have bytes, base shadow${t}sw 0x$want"; exit 1; }
done

echo "[*] linking the Mac overrides over the base"
m68k-cbm-sysv4-ld -r -o "$OUT" "$W/base.weak" $OBJS
python3 "$MAC/patch_nread.py" "$OUT"
python3 "$K/guest/patch_vec.py" "$OUT" "$W/guest/gates.lst"

# Each override must now be one strong definition inside our objects,
# which follow the whole base .text.
btext=$(m68k-linux-gnu-size -A "$BASE" | awk '$1==".text"{print $2}')
bad=0
for s in $OVR; do
	v=$(m68k-linux-gnu-nm "$OUT" | awk -v s="$s" '$3==s && $2=="T"{print $1}')
	n=$(m68k-linux-gnu-nm "$OUT" | awk -v s="$s" '$3==s && $2 ~ /^[TWV]$/' | wc -l)
	if [ -z "$v" ] || [ "$n" -ne 1 ] || [ $((0x$v)) -lt "$btext" ]; then
		echo "[FAIL] $s did not bind to the Mac object"; bad=1
	fi
done
bdata=$(m68k-linux-gnu-size -A "$BASE" | awk '$1==".data"{print $2}')
for s in $OVRD; do
	v=$(m68k-linux-gnu-nm "$OUT" | awk -v s="$s" '$3==s && $2=="D"{print $1}')
	n=$(m68k-linux-gnu-nm "$OUT" | awk -v s="$s" '$3==s && $2 ~ /^[DdVv]$/' | wc -l)
	if [ -z "$v" ] || [ "$n" -ne 1 ] || [ $((0x$v)) -lt "$bdata" ]; then
		echo "[FAIL] $s did not bind to the Mac object"; bad=1
	fi
done
[ "$bad" -eq 0 ] || exit 1
echo "[OK] $(echo $OVR $OVRD | wc -w) overrides bound"

u=$(m68k-linux-gnu-nm -u "$OUT" | awk '{print $2}' | grep -vxE 'etext|edata|end' || true)
if [ -n "$u" ]; then echo "[FAIL] unresolved in $OUT:"; echo "$u"; exit 1; fi
echo "[OK] unresolved (ld -r): 0 besides etext/edata/end"

echo "[*] relocation validator (AMIX loader model)"
python3 "$PORT/src/check_relink_relocs.py" "$OUT" > "$W/validator.log" || {
	tail -20 "$W/validator.log"; exit 1; }
tail -1 "$W/validator.log"

echo "[*] final link at 0x10000"
m68k-elf-ld -T "$MAC/mac.ld" -Map "$OUT.map" -o "$OUT.elf" "$OUT"
python3 "$MAC/patch_s5pages.py" -c "$OUT.elf"
[ $MODE = stock ] || python3 "$MAC/vtop/patch_vtop.py" -c "$OUT.elf"
[ $MODE = stock ] || python3 "$MAC/patch_kvmpages.py" -c "$OUT.elf"

echo "[*] kernel symbol table into dlm_ksym"
"$W/dlm/mkksym" "$OUT.elf"
"$W/dlm/mkksym" -c "$OUT.elf"
m68k-elf-readelf -h "$OUT.elf" | grep -E 'Type|Entry'
m68k-elf-size "$OUT.elf"
echo "[OK] built $OUT and $OUT.elf"
