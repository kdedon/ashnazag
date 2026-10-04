#!/bin/sh
# build.sh -- RAM-disk driver and embedded root image objects.
#
#   sh ramdisk/build.sh [image]
#
# image: filesystem image to link in (default build/root.img; none -> empty).
# Out:   build/rd.o, build/rdimage.o
set -e

RD=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$RD/../../.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

B="$RD/build"
mkdir -p "$B"
IMG="${1:-$B/root.img}"

m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -m68040 $RD_CFLAGS -c "$RD/rd.c" -o "$B/rd.o"

# mac_rd_config() runs before BSS is cleared
if m68k-linux-gnu-nm "$B/rd.o" | grep -q ' [bBC] '; then
	echo "[FAIL] rd.o has BSS/common symbols"; exit 1
fi

if [ -s "$IMG" ]; then
	echo "[*] embedding $IMG ($(wc -c < "$IMG") bytes)"
	cp "$IMG" "$B/rdimage.bin"
	(cd "$B" && m68k-linux-gnu-objcopy -I binary -O elf32-m68k -B m68k \
		--rename-section .data=.data,alloc,load,data,contents \
		--set-section-alignment .data=4 \
		--redefine-sym _binary_rdimage_bin_start=rd_image \
		--redefine-sym _binary_rdimage_bin_end=rd_image_end \
		--strip-symbol _binary_rdimage_bin_size \
		rdimage.bin rdimage.o)
else
	echo "[*] no image: empty rd_image (boot-record ramdisk only)"
	printf '\t.data\n\t.globl rd_image\n\t.globl rd_image_end\nrd_image:\nrd_image_end:\n' \
		> "$B/rdimage.s"
	m68k-cbm-sysv4-gcc -c "$B/rdimage.s" -o "$B/rdimage.o"
fi
m68k-linux-gnu-nm "$B/rdimage.o"
echo "[OK] $B/rd.o $B/rdimage.o"
