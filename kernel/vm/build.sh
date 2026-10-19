#!/bin/sh
# build.sh BASE OUTDIR -- compiles OUTDIR/vm.o for the AMIX base BASE and
# prints the objcopy flags that let it replace the stock functions:
# virtual swap (swapadd, swapdel, swap_free, anon_resv, anon_unresv) on every kernel, a heap
# that grows (sptalloc, sptfree) on the 68040/68060 ones.
# VM_CHAIN: functions the caller already wraps, its wrappers calling
# __amix_<name>; ours become __amix_<name> and the caller weakens.
# Needs the AMIX cross environment and PATH of the relink scripts.
set -e
V=$(cd "$(dirname "$0")" && pwd)
BASE=$1 W=$2
CC="m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68020"
FN="swapadd:t swapdel:t swap_free:T anon_resv:T anon_unresv:T"
if m68k-linux-gnu-nm "$BASE" | grep -q ' [DdBb] kptr040$'; then
	$CC -DVS_PGSHIFT=12 -c "$V/vswap.c" -o "$W/vswap.o" >&2
	$CC -c "$V/kheap.c" -o "$W/kheap.o" >&2
	m68k-cbm-sysv4-ld -r -o "$W/vm.o" "$W/vswap.o" "$W/kheap.o" >&2
	FN="$FN sptalloc:T sptfree:T kmem_alloc:T"
else
	$CC -DVS_PGSHIFT=11 -c "$V/vswap.c" -o "$W/vm.o" >&2
fi
f=
for s in $FN; do
	n=${s%:*} t=${s#*:}
	v=$(m68k-linux-gnu-nm "$BASE" | awk -v n="$n" -v t="$t" '$3==n && $2==t{print $1}')
	[ -n "$v" ] || { echo "[FAIL] no $n in $BASE" >&2; exit 1; }
	f="$f --add-symbol __vm_$n=.text:0x$v,function,global"
	case " $VM_CHAIN " in
	*" $n "*) m68k-linux-gnu-objcopy --redefine-sym $n=__amix_$n "$W/vm.o"; continue ;;
	esac
	[ $t = t ] && f="$f --globalize-symbol $n"
	f="$f --weaken-symbol $n"
done
echo "$f"
