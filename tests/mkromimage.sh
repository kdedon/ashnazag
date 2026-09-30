#!/bin/sh
# mkromimage.sh -- ROM-boot disk: the small test image with a kernel that has
# the test root linked in.
#
#   [KDIR=kernel-tree] [WORK=dir] [TESTKB=2048] sh tests/mkromimage.sh
#
# A/UX Startup passes no RAM-disk record and loads at most 4000 KB, hence the
# linked-in, smaller root, without t_aux's A/UX files (740 KB).  The kernel
# is relinked in a copy under WORK.
# Out: build/rom/unix-test.coff, build/rom/q800-test-small-tests.img.
set -e

T=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
WORK=${WORK:-$T/build/romwork}
R=$T/build/rom
TB=$AUX/toolchain/bin
SRCIMG=$AUX/images/q800-test-small.img

mkdir -p "$R" "$WORK/home"
echo "[*] relinking in $WORK/kernel"
rm -rf "$WORK/kernel"
mkdir -p "$WORK/kernel"
tar -C "$KDIR" --exclude=./mac/ramdisk/build/core --exclude=./patch/payload/root \
	-cf - . | tar -C "$WORK/kernel" -xf -
ln -sfn "$AUX/toolchain" "$WORK/toolchain"

# ksyms addresses shift with the image size only, so the second link is final
K=$KDIR/build/unix-mac.elf
for pass in 1 2; do
	KERNEL=$K IMG=$R/testroot.img TESTKB=${TESTKB:-2048} NOAUX=1 sh "$T/mktestroot.sh"
	cp "$R/testroot.img" "$WORK/kernel/mac/ramdisk/build/root.img"
	nice -n 19 sh "$WORK/kernel/mac/relink-mac.sh" > "$R/relink.log" 2>&1 ||
		{ tail -20 "$R/relink.log"; exit 1; }
	K=$WORK/kernel/build/unix-mac.elf
done
nm "$K" | awk '$3 ~ /^(freemem|availrmem|availsmem|lbolt|fpu_present|anoninfo|ticks_til_clock|mac_ticks|dlm_inited|sn_nintr|sn_nslot|guest_loading|rd_unit)$/ { print $3, $1 }' |
	cmp -s - "$T/build/ksyms" || { echo "[FAIL] ksyms moved between passes"; exit 1; }
nice -n 19 sh "$WORK/kernel/mac/boot/mkcoff.sh" "$K" "$R/unix-test.coff" >> "$R/relink.log" 2>&1
"$WORK/kernel/mac/boot/build/elf2coff" -c "$R/unix-test.coff" | tail -1 | grep ACCEPTED ||
	{ echo "[FAIL] unix-test.coff rejected"; exit 1; }
cp "$WORK/kernel/build/unix-mac.elf" "$R/unix-test.elf"
sz=$(wc -c < "$R/unix-test.coff")
[ "$sz" -le $((4000 * 1024)) ] || { echo "[FAIL] unix-test.coff is $sz bytes, over 4000 KB"; exit 1; }

echo "[*] disk image"
cp "$SRCIMG" "$R/q800-test-small-tests.img"
HOME=$WORK/home
export HOME
"$TB/hmount" "$R/q800-test-small-tests.img" 1 > /dev/null
"$TB/hdel" ":unix.coff"
"$TB/hcopy" -r "$R/unix-test.coff" ":unix.coff"
"$TB/hattrib" -t COFF -c SASH ":unix.coff"
"$TB/hcopy" -r ":unix.coff" "$WORK/check.coff"
"$TB/humount"
cmp "$WORK/check.coff" "$R/unix-test.coff"
echo "[OK] $R/q800-test-small-tests.img (unix.coff $sz bytes)"
