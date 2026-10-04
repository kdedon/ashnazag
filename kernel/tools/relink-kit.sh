#!/bin/sh
# relink-kit.sh -- relink AMIX 2.1c's /stand/unix from the link kit's shipped objects.
#
#   sh tools/relink-kit.sh [outdir]          (default: kernel/amix-2.1c)
#
# The 2.1 tape's stand/unix is the retail 2.1 kernel; the kit under usr/sys carries the 2.1c
# objects.  This does what the kit's own Makefiles do (ld -r in their order) with the cross
# linker, using only shipped objects.  The one compiled file is amiga/config/unix.c, with the
# values the retail kernel was configured with (root c6d0s2, swap 0xf000 blocks).
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$HERE/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
S="${KIT:-$AUX/toolchain/amix-root/usr/sys}"
OUT="${1:-$HERE/amix-2.1c}"
W="$OUT/work"
LD=m68k-cbm-sysv4-ld
mkdir -p "$W" "$OUT/stand"

lr() {	# out, dir, objs...
	_o="$1"; _d="$2"; shift 2
	( cd "$_d" && $LD -r -o "$_o" "$@" )
}

lr "$W/amiga-ml.o"      "$S/amiga/ml"      ttrap.o vec.o syms.o
lr "$W/amiga-console.o" "$S/amiga/console" kio.o c0.o c1.o c3.o memory.o scrdev.o screen.o \
	usa1.kmap.o font.o topaz.o
lr "$W/amiga-alien.o"   "$S/amiga/alien"   ct.o dd.o a2090.o a2091.o a3091.o sd.o sdpart.o \
	physdsk.o scsi.o
lr "$W/amiga-driver.o"  "$S/amiga/driver"  acia.o amiga.o ben.o bb.o cl.o dummy.o hd.o jb.o \
	machid.o par.o ram.o ql.o sl.o slip.o tiga.o audio.o aen/exp
lr "$W/amiga-floppy.o"  "$S/amiga/floppy"  $(sed -n 's/^OBJ[ 	]*=[ 	]*//p' "$S/amiga/floppy/Makefile")
lr "$W/amiga-kernel.o"  "$S/amiga/kernel"  servant.o support.o
lr "$W/amiga.o" "$W" amiga-ml.o amiga-console.o amiga-alien.o amiga-driver.o amiga-floppy.o \
	amiga-kernel.o
lr "$W/master.o" "$S/master.d" arp.o disp.o filesys.o gentty.o hrt.o icd.o ip.o kernel.o \
	kmacct.o krpc.o llcloop.o log.o msg.o opts.o prf.o ptem.o ptm.o rt.o sad.o sem.o shm.o \
	sockmod.o sp.o sxt.o tcp.o timod.o tirdwr.o ts.o udp.o xt.o stubs.o
lr "$W/fs.o" "$S/fs" fs.exp bfs/exp s5/exp xnamfs/exp ufs/exp nfs/exp rfs/exp
lr "$W/local.o" "$S/local" empty.o res.o

m68k-cbm-sysv4-gcc -c -O -DSYSV -D_KERNEL -I"$S" -I"$AUX/toolchain/amix-root/usr/include" \
	-DROOTDEV=0x480016 -DDUMPDEV=0x400004 -DCHAR_SPECIAL=0 -DDO_SYSDUMP=0 \
	-DNO_INTERACTIVE=0 -DSWAPDEV='"/dev/dsk/c6d0s2"' -DNSWAP=0xf000 -DSWPLO=0 \
	"$S/amiga/config/unix.c" -o "$W/unix.o"

( cd "$S" && $LD -r -o "$OUT/stand/unix" ml/exp "$W/amiga.o" io/exp os/exp "$W/fs.o" \
	"$W/master.o" vm/exp exec/exp disp/exp ktli/exp klm/exp rpc/exp des/exp netinet/exp \
	"$W/local.o" "$W/unix.o" )

u=$(m68k-cbm-sysv4-nm -u "$OUT/stand/unix" | grep -vwE 'etext|edata|end' || true)
[ -z "$u" ] || { echo "[FAIL] unresolved:"; echo "$u"; exit 1; }
echo "[OK] $OUT/stand/unix  sha256 $(sha256sum "$OUT/stand/unix" | cut -d' ' -f1)"
m68k-cbm-sysv4-nm "$OUT/stand/unix" | grep -w fpu_setup
