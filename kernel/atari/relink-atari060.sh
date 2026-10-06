#!/bin/sh
# relink-atari060.sh -- Atari Falcon kernel for a 68040 or 68060 (CT60/CT63)
# from the port's 040/060 image.
#
#   sh atari/relink-atari060.sh [base [out]]
#
# base: the port's relocatable kernel (default amix-040-060-port/build/unix-040).
# out:  build/unix-atari060 (ld -r image) and build/unix-atari060.elf
#       (fully linked at 0x1000, a jump to the entry first, with a
#       relocation table that lets it move itself into FastRAM).
# The Atari objects of the 030 kernel, built for the 040/060, over the
# base the Mac kernel uses, with the same base fixes (4 KB s5 pages, vtop,
# page-table waits, page array in a cached window, segkmap writes).  The
# base keeps its 060 layer: FPSP, ISP, access-error frames, PCR.
# The compiler turns a constant division into a 64-bit multiply, which the
# 060 emulates through a trap.  Objects with such code build -m68000 and
# call 32-bit helpers; chk64.py fails the build if one of ours still has any.
set -e

A=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$A/.." && pwd)
AUX=$(cd "$K/.." && pwd)
PORT="$K/amix-040-060-port"
MAC="$K/mac"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

BASE="${1:-$PORT/build/unix-040}"
OUT="${2:-$K/build/unix-atari060}"
W="$K/build/atari060"
mkdir -p "$W"
[ -f "$BASE" ] || { echo "[FAIL] no base image: $BASE"; exit 1; }
m68k-linux-gnu-nm "$BASE" | grep -q ' [DdBb] kptr040$' ||
	{ echo "[FAIL] $BASE has no 040/060 VM layer"; exit 1; }

CC="m68k-cbm-sysv4-gcc -m68040"
AS="$CC -Wa,--defsym,ATA060=1"
CFLAGS="$AMIX_KERNEL_CFLAGS -m68040 -DATA060 -Wall -Wno-comment"
SOFTMUL=-m68000
cc_nobss() {
	o="$W/$(basename "$1" .c).o"
	$CC $CFLAGS $2 -c "$1" -o "$o" 2> "$o.warn" || { cat "$o.warn"; exit 1; }
	[ -s "$o.warn" ] && { cat "$o.warn"; echo "[FAIL] $1: warnings"; exit 1; }
	[ -z "$3" ] || ! m68k-linux-gnu-nm "$o" | grep -q ' [bBC] ' ||
		{ echo "[FAIL] $o has BSS/common symbols"; exit 1; }
}

echo "[*] Atari platform objects (040/060)"
for s in ataboot ataentry ataintr pstartata ata060math; do
	$AS -c "$A/$s.s" -o "$W/$s.o"
done
$CC -c "$MAC/devtab/cfgorig.s" -o "$W/cfgorig.o"
# config(), the shim and the console run before BSS is cleared
for c in "$A/ikbd.c" "$A/atadevsw.c" "$MAC/video/fbfont.c"; do
	cc_nobss "$c" "" nobss
done
for c in "$A/ataconf.c" "$MAC/video/fbcons.c"; do
	cc_nobss "$c" "$SOFTMUL" nobss
done
cc_nobss "$A/atacons.c"
DSINC="-DDS_ATARI -I$MAC/display -I$MAC/video -I$MAC/adb"
for c in "$MAC/display/ds.c" "$MAC/display/dsdev.c" "$MAC/display/dsseg.c" "$A/atads.c"; do
	cc_nobss "$c" "-fno-common $DSINC $SOFTMUL"
done
for c in ataide ahdi; do
	cc_nobss "$A/$c.c" "-I$AMIX_ROOT/usr/sys/amiga/alien"
done
cc_nobss "$A/atartc.c" "-I$AMIX_ROOT/usr/sys/amiga/alien $SOFTMUL"
cc_nobss "$MAC/s5dir/uiomod.c"
$CC -c "$MAC/vtop/vtop.s" -o "$W/vtop.o"
$CC -c "$MAC/ptalloc/ptalloc.s" -o "$W/ptalloc.o"
m68k-linux-gnu-objcopy --redefine-sym hat_ptalloc=ata_ptalloc_mac "$W/ptalloc.o"

echo "[*] RAM disk"
sh "$MAC/ramdisk/build.sh" > "$W/ramdisk.log"
RDB="$MAC/ramdisk/build"
RDDEF="-Dmac_rd_config=ata_rd_config -Dmac_bi=ata_bi -Dmac_bilen=ata_bilen
-Dmac_puts=ata_puts -Dmac_puthex=ata_puthex"
$CC $AMIX_KERNEL_CFLAGS $RDDEF -c "$MAC/ramdisk/rd.c" -o "$W/rd.o"
if m68k-linux-gnu-nm "$W/rd.o" | grep -q ' [bBC] '; then
	echo "[FAIL] rd.o has BSS/common symbols"; exit 1
fi
[ -s "$RDB/rdimage.bin" ] || { echo "[FAIL] no root image: $RDB/root.img"; exit 1; }
echo "[OK] root image $(wc -c < "$RDB/rdimage.bin") bytes"

echo "[*] loadable modules and guest processes"
sh "$K/dlm/build.sh" -k "$W/dlm"
sh "$K/guest/build.sh" -k "$BASE" "$W/guest"

OBJS="$W/dlm/dlm.o $W/guest/guest.o $W/ataentry.o $W/ataintr.o $W/pstartata.o $W/ata060math.o $W/cfgorig.o
$W/ataconf.o $W/ikbd.o $W/fbcons.o $W/fbfont.o $W/atadevsw.o $W/atacons.o $W/ds.o $W/dsdev.o
$W/dsseg.o $W/atads.o $W/ataide.o $W/ahdi.o $W/atartc.o $W/rd.o $RDB/rdimage.o
$W/uiomod.o $W/vtop.o $W/ptalloc.o"

OVR="pstart config config_orig putchar getchar callrom sysdump haltsys rtnfirm
hw_clkstart clkreld p1int p2int p3int p4int p5int p6int parinit qlintr slpoll
autocon delayus ramopen ramclose ramstrategy ramprint ramsize backtrace inituname
sdopen sdqueue sdhardwarename sdpartition sdvalid sdblkno sddevsize
clkset dmainit ev_config ev_fork ev_exec ev_exit sendsig valid_usr_range fsig mdboot
vtop hat_ptalloc uiomove unt_latch page_free segu_get swapinub"
OVRD="coinfo cdevsw bdevsw execsw fmodsw"
ALIAS="inituname:T sendsig:T valid_usr_range:T fsig:T execsw:D fmodsw:D
hat_ptalloc:T uiomove:T unt_latch:T page_free:T segu_get:T swapinub:T"

echo "[*] weakening the Amiga platform entry points"
WEAK="--globalize-symbol freemem_wait"
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
for a in $ALIAS; do
	s=${a%:*} t=${a#*:}
	off=$(m68k-linux-gnu-nm "$BASE" | awk -v s="$s" -v t="$t" '$3==s && $2==t{print $1}')
	[ -n "$off" ] || { echo "[FAIL] no $s to alias"; exit 1; }
	if [ "$t" = T ]; then sec=.text kind=function; else sec=.data kind=object; fi
	WEAK="$WEAK --add-symbol __amix_$s=$sec:0x$off,$kind,global"
done
for t in c b; do
	want=$(m68k-linux-gnu-nm "$BASE" | awk -v s=shadow${t}sw '$3==s{print $1}')
	have=$(m68k-linux-gnu-nm -S "$W/atadevsw.o" | awk -v s=${t}devsw '$4==s{print $2}')
	[ -n "$want" ] && [ $((0x$want)) -eq $((0x${have:-0})) ] || {
		echo "[FAIL] ${t}devsw is 0x$have bytes, base shadow${t}sw 0x$want"; exit 1; }
done
m68k-linux-gnu-objcopy $WEAK "$BASE" "$W/base.weak"
python3 "$MAC/patch_s5pages.py" "$W/base.weak"
python3 "$MAC/vtop/patch_vtop.py" "$W/base.weak"
python3 "$MAC/patch_kvmpages.py" "$W/base.weak"

echo "[*] no 64-bit multiply or divide in the Atari objects"
python3 "$A/chk64.py" $W/ataentry.o $W/ataintr.o $W/pstartata.o $W/ata060math.o $W/cfgorig.o \
	$W/ataconf.o $W/ikbd.o $W/fbcons.o $W/fbfont.o $W/atadevsw.o $W/atacons.o \
	$W/ds.o $W/dsdev.o $W/dsseg.o $W/atads.o $W/ataide.o $W/ahdi.o $W/atartc.o \
	$W/uiomod.o $W/vtop.o $W/ptalloc.o $W/rd.o

echo "[*] linking the Atari overrides over the base"
m68k-cbm-sysv4-ld -r -o "$OUT" "$W/base.weak" $OBJS
python3 "$MAC/patch_nread.py" "$OUT"
python3 "$K/guest/patch_vec.py" "$OUT" "$W/guest/gates.lst"

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

# Link twice: the first link gives the relocation sites, the second
# carries their table, which sits past .data and so moves none of them.
link() {
	(cd "$W" && m68k-linux-gnu-objcopy -I binary -O elf32-m68k -B m68k \
		--rename-section .data=.reltab,alloc,load,data,contents \
		--set-section-alignment .reltab=4 --strip-symbol _binary_reltab_bin_start \
		--strip-symbol _binary_reltab_bin_end --strip-symbol _binary_reltab_bin_size \
		reltab.bin reltab.o)
	m68k-elf-ld -q -T "$A/atari060.ld" -Map "$OUT.map" -o "$OUT.elf" \
		"$W/ataboot.o" "$OUT" "$W/reltab.o"
	python3 "$MAC/patch_s5pages.py" -c "$OUT.elf"
	python3 "$MAC/vtop/patch_vtop.py" -c "$OUT.elf"
	python3 "$MAC/patch_kvmpages.py" -c "$OUT.elf"
	"$W/dlm/mkksym" "$OUT.elf"
}
echo "[*] final link at 0x1000"
python3 "$A/mkreltab.py" -e "$W/reltab.bin"
link
python3 "$A/mkreltab.py" "$OUT.elf" "$W/reltab.bin"
link
"$W/dlm/mkksym" -c "$OUT.elf"
python3 "$A/mkreltab.py" -c "$OUT.elf"
m68k-elf-readelf -h "$OUT.elf" | grep -E 'Type|Entry'
m68k-elf-size "$OUT.elf"
python3 "$A/chk64.py" -r "$OUT.elf"
echo "[OK] built $OUT and $OUT.elf"
