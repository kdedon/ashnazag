#!/bin/sh
# relink-atari.sh -- Atari Falcon030 kernel from the stock AMIX 2.1p2a image.
#
#   sh atari/relink-atari.sh [base [out]]
#
# base: the relocatable stock kernel (default amix-2.1p2a/stand/unix).
# out:  build/unix-atari030 (ld -r image) and build/unix-atari030.elf
#       (fully linked at 0x1000, a jump to the entry first).
# Weakens the Amiga platform entry points, raises the inlined splhi to
# IPL 6, links the Atari objects over the base, checks that every
# override bound to our object, validates, then links.  Root is the RAM
# disk with the image from mac/ramdisk/build/root.img; the console tty
# is the screen and the IKBD keyboard; the display service (majors 51-53)
# shares that screen and keyboard.
set -e

A=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$A/.." && pwd)
AUX=$(cd "$K/.." && pwd)
PORT="$K/amix-040-060-port"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

BASE="${1:-$K/amix-2.1p2a/stand/unix}"
OUT="${2:-$K/build/unix-atari030}"
W="$K/build/atari"
SPL_SITES=424
mkdir -p "$W"
[ -f "$BASE" ] || { echo "[FAIL] no base image: $BASE"; exit 1; }

CC="m68k-cbm-sysv4-gcc -m68030"
CFLAGS="$AMIX_KERNEL_CFLAGS -Wall -Wno-comment"
echo "[*] Atari platform objects"
for s in ataboot ataentry ataintr; do
	$CC -c "$A/$s.s" -o "$W/$s.o"
done
for c in "$A/ataconf.c" "$A/ikbd.c" "$A/atadevsw.c" "$K/mac/video/fbcons.c" "$K/mac/video/fbfont.c"; do
	o="$W/$(basename "$c" .c).o"
	$CC $CFLAGS -c "$c" -o "$o" 2> "$o.warn" || { cat "$o.warn"; exit 1; }
	[ -s "$o.warn" ] && { cat "$o.warn"; echo "[FAIL] $c: warnings"; exit 1; }
	# config() and the console run before BSS is cleared
	if m68k-linux-gnu-nm "$o" | grep -q ' [bBC] '; then
		echo "[FAIL] $o has BSS/common symbols"; exit 1
	fi
done
# the console tty runs only after main() has cleared BSS
$CC $CFLAGS -c "$A/atacons.c" -o "$W/atacons.o" 2> "$W/atacons.o.warn" || {
	cat "$W/atacons.o.warn"; exit 1; }
[ -s "$W/atacons.o.warn" ] && { cat "$W/atacons.o.warn"; echo "[FAIL] atacons.c: warnings"; exit 1; }

# the display service and its input, after main() has cleared BSS
DSINC="-DDS_ATARI -I$K/mac/display -I$K/mac/video -I$K/mac/adb"
for c in "$K/mac/display/ds.c" "$K/mac/display/dsdev.c" "$K/mac/display/dsseg.c" "$A/atads.c"; do
	o="$W/$(basename "$c" .c).o"
	$CC $CFLAGS -fno-common $DSINC -c "$c" -o "$o" 2> "$o.warn" || { cat "$o.warn"; exit 1; }
	[ -s "$o.warn" ] && { cat "$o.warn"; echo "[FAIL] $c: warnings"; exit 1; }
done

# the disk layer runs only after main() has cleared BSS
for c in ataide ahdi atartc; do
	$CC $CFLAGS -I"$AMIX_ROOT/usr/sys/amiga/alien" -c "$A/$c.c" -o "$W/$c.o" 2> "$W/$c.o.warn" || {
		cat "$W/$c.o.warn"; exit 1; }
	[ -s "$W/$c.o.warn" ] && { cat "$W/$c.o.warn"; echo "[FAIL] $c.c: warnings"; exit 1; }
done

# FPU emulator from the pinned tarball, with its glue.  The glue maps only
# CPU types 40 and 60 and counts any other as an error; the emulator runs
# the same on every CPU, so the glue reads 40.
echo "[*] FPU emulator"
FP="$PORT/src"
NETBSD_SYSSRC="$AUX/toolchain/dl/syssrc.tgz"
. "$PORT/tools/netbsd-pin.sh"
netbsd_syssrc_verify "$NETBSD_SYSSRC" > /dev/null
FS="$W/fpe-src"
rm -rf "$FS" "$W/fpe-obj"; mkdir -p "$FS/include/m68k" "$FS/include/sys" "$W/fpe-obj"
tar -xzf "$NETBSD_SYSSRC" --exclude=CVS -C "$FS" --strip-components=6 usr/src/sys/arch/m68k/fpe
tar -xzf "$NETBSD_SYSSRC" --exclude=CVS -C "$FS/include/m68k" --strip-components=6 \
	usr/src/sys/arch/m68k/include/cpuframe.h usr/src/sys/arch/m68k/include/fpreg.h \
	usr/src/sys/arch/m68k/include/ieee.h
tar -xzf "$NETBSD_SYSSRC" --exclude=CVS -C "$FS/include/sys" --strip-components=4 usr/src/sys/sys/ieee754.h
python3 "$FP/mk_fpe_cc.py" "$W/fpe-cc" > "$W/fpe-cc.log" 2>&1 || { cat "$W/fpe-cc.log"; exit 1; }
FCC=$(tail -1 "$W/fpe-cc.log")
FCF="$(echo "$AMIX_KERNEL_CFLAGS" | sed 's/-m68020/-m68030/')"
FINC="-I$FP/fpe-compat -I$FS/include -I$FS"
FPEOBJS=
for f in "$FS"/*.c; do
	o="$W/fpe-obj/$(basename "$f" .c).o"
	"$FCC" $FCF $FINC -Dpanic=fpe_panic -Dcopyin=fpe_copyin -Dcopyout=fpe_copyout -c "$f" -o "$o"
	FPEOBJS="$FPEOBJS $o"
done
[ $(echo $FPEOBJS | wc -w) -eq 20 ] || { echo "[FAIL] expected 20 FPE objects"; exit 1; }
"$FCC" $FCF $FINC -Dcputype=fpe_glue_cputype -c "$FP/fpe_glue.c" -o "$W/fpe-obj/fpe_glue.o"
$CC -c "$A/fpe030.s" -o "$W/fpe030.o"
$CC -c "$FP/fpe040.s" -o "$W/fpe040.o"
echo "[OK] 20 emulator objects and glue"

echo "[*] RAM disk"
sh "$K/mac/ramdisk/build.sh" > "$W/ramdisk.log"
RDB="$K/mac/ramdisk/build"
RDDEF="-Dmac_rd_config=ata_rd_config -Dmac_bi=ata_bi -Dmac_bilen=ata_bilen
-Dmac_puts=ata_puts -Dmac_puthex=ata_puthex"
$CC $AMIX_KERNEL_CFLAGS $RDDEF -c "$K/mac/ramdisk/rd.c" -o "$W/rd.o"
if m68k-linux-gnu-nm "$W/rd.o" | grep -q ' [bBC] '; then
	echo "[FAIL] rd.o has BSS/common symbols"; exit 1
fi
[ -s "$RDB/rdimage.bin" ] || { echo "[FAIL] no root image: $RDB/root.img"; exit 1; }
echo "[OK] root image $(wc -c < "$RDB/rdimage.bin") bytes"

OBJS="$W/ataentry.o $W/ataintr.o $W/ataconf.o $W/ikbd.o $W/fbcons.o $W/fbfont.o
$W/atadevsw.o $W/atacons.o $W/ds.o $W/dsdev.o $W/dsseg.o $W/atads.o $W/ataide.o $W/ahdi.o $W/atartc.o $W/rd.o $RDB/rdimage.o
$W/fpe030.o $FPEOBJS $W/fpe-obj/fpe_glue.o $W/fpe040.o"

OVR="config putchar getchar callrom sysdump haltsys rtnfirm hw_clkstart clkreld
p1int p2int p3int p4int p5int p6int parinit qlintr slpoll autocon delayus
ramopen ramclose ramstrategy ramprint ramsize backtrace fpu_setup inituname
sdopen sdqueue sdhardwarename sdpartition sdvalid sdblkno sddevsize
prhasfp fpuinit fpu_save fpu_restore setregs clkset"
# Amiga data replaced: the console streamtab and the device switches.
OVRD="coinfo cdevsw bdevsw"
# Stock bodies kept under __amix_<name> for the wrappers.
ALIAS="fpu_setup inituname"
# Stock bodies the emulator chains to, as <name>_fpe_orig.
FPEORIG="fpuinit fpu_save fpu_restore setregs"

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
for s in $ALIAS; do
	off=$(m68k-linux-gnu-nm "$BASE" | awk -v s="$s" '$3==s && $2=="T"{print $1}')
	WEAK="$WEAK --add-symbol __amix_$s=.text:0x$off,function,global"
done
for t in c b; do
	want=$(m68k-linux-gnu-nm "$BASE" | awk -v s=shadow${t}sw '$3==s{print $1}')
	have=$(m68k-linux-gnu-nm -S "$W/atadevsw.o" | awk -v s=${t}devsw '$4==s{print $2}')
	[ -n "$want" ] && [ $((0x$want)) -eq $((0x${have:-0})) ] || {
		echo "[FAIL] ${t}devsw is 0x$have bytes, base shadow${t}sw 0x$want"; exit 1; }
done
for s in $FPEORIG; do
	off=$(m68k-linux-gnu-nm "$BASE" | awk -v s="$s" '$3==s && $2=="T"{print $1}')
	WEAK="$WEAK --add-symbol ${s}_fpe_orig=.text:0x$off,function,global"
done
WEAK="$WEAK --globalize-symbol trapsig"
m68k-linux-gnu-objcopy $WEAK "$BASE" "$W/base.weak"
python3 "$A/patch_spl.py" -p "$W/base.weak" $SPL_SITES

echo "[*] linking the Atari overrides over the base"
m68k-cbm-sysv4-ld -r -o "$OUT" "$W/base.weak" $OBJS

btext=$(m68k-linux-gnu-size -A "$BASE" | awk '$1==".text"{print $2}')
bad=0
for s in $OVR; do
	v=$(m68k-linux-gnu-nm "$OUT" | awk -v s="$s" '$3==s && $2=="T"{print $1}')
	n=$(m68k-linux-gnu-nm "$OUT" | awk -v s="$s" '$3==s && $2 ~ /^[TWV]$/' | wc -l)
	if [ -z "$v" ] || [ "$n" -ne 1 ] || [ $((0x$v)) -lt "$btext" ]; then
		echo "[FAIL] $s did not bind to the Atari object"; bad=1
	fi
done
bdata=$(m68k-linux-gnu-size -A "$BASE" | awk '$1==".data"{print $2}')
for s in $OVRD; do
	v=$(m68k-linux-gnu-nm "$OUT" | awk -v s="$s" '$3==s && $2=="D"{print $1}')
	n=$(m68k-linux-gnu-nm "$OUT" | awk -v s="$s" '$3==s && $2 ~ /^[DdVv]$/' | wc -l)
	if [ -z "$v" ] || [ "$n" -ne 1 ] || [ $((0x$v)) -lt "$bdata" ]; then
		echo "[FAIL] $s did not bind to the Atari object"; bad=1
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

echo "[*] final link at 0x1000"
m68k-elf-ld -T "$A/atari.ld" -Map "$OUT.map" -o "$OUT.elf" "$W/ataboot.o" "$OUT"
python3 "$A/patch_spl.py" -c "$OUT.elf" $SPL_SITES
m68k-elf-readelf -h "$OUT.elf" | grep -E 'Type|Entry'
m68k-elf-size "$OUT.elf"
echo "[OK] built $OUT and $OUT.elf"
