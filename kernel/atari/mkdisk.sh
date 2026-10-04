#!/bin/sh
# mkdisk.sh -- build the Falcon IDE disk image (raw): AHDI root sector
# with boot code, AXB (loader and kernel image), AXR (root, s1), AXS
# (swap, s2) and AXU (/home, s3).  TOS 3/4 boot it from power on; or
# run-hatari.sh -k KERNEL -a 'root=c0d0s1' -d COPY-OF-IMAGE.
#
#   sh mkdisk.sh [kernel.elf [out.img]]
#
# Defaults: kernel/build/unix-atari030.elf, kernel/build/atari/disk/
# falcon-disk.img.  The root is the Mac disk root (root.manifest, ufs;
# ROOTFS=s5 for s5) with Atari changes; this kernel becomes /stand/unix.
# DISKMB (default 512), ROOTMB (default 128), SWAPMB (default 64, all
# of it used); /home gets the rest, from 4 MB on.  GEM=1: the
# rest (at most 508 MB) is a TOS FAT partition (BGM, s4) instead of /home.
# BOOTARGS: kernel command line (default root=c?d0s1, ? = boot unit).
# The tape segments come from the Mac build or $AMIX_TAPE.
set -e

A=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$A/.." && pwd)
DR=$K/mac/diskroot
KERNEL=${1:-$K/build/unix-atari030.elf}
W=$K/build/atari/disk
OUT=${2:-$W/falcon-disk.img}
DISKMB=${DISKMB:-512}
ROOTMB=${ROOTMB:-128}
SWAPMB=${SWAPMB:-64}
BOOTARGS=${BOOTARGS:-root=c?d0s1}
GEM=${GEM:-0}
PY="nice -n 19 python3"

case $KERNEL in /*) ;; *) KERNEL=$PWD/$KERNEL ;; esac
mkdir -p "$W/src/build"
ln -sfn "$DR/etc" "$W/src/etc"
ln -sfn "$A/etc" "$W/src/atari"
[ -d "$W/src/build/tape" ] || [ ! -d "$DR/build/tape" ] ||
	cp -r "$DR/build/tape" "$W/src/build/tape"
(. "$K/../toolchain/src/gcc-cross-amix/build/env.sh"
 m68k-cbm-sysv4-gcc -O -o "$W/src/build/setclk" "$A/setclk.c")
# no Mac display test; Atari node name; the RTC through /dev/clock
{ grep -v -e '^f /usr/bin/dstest' -e '^r /dev/clock$' "$DR/root.manifest"
  echo 'f /etc/nodename	644 0 3 atari/nodename'
  echo 'f /etc/sysinit	744 0 3 atari/sysinit'
  echo 'f /usr/amiga/bin/setclk 755 0 3 build/setclk'
  echo 'd /home 755 0 3'
  echo 'f /etc/vfstab 744 0 3 vfstab'; } > "$W/root.manifest"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
	/dev/dsk/c0d0s1 /dev/rdsk/c0d0s1 / ${ROOTFS:-ufs} 1 no - \
	proc - /proc proc 0 no - \
	fd - /dev/fd fd 0 no - > "$W/src/vfstab"
[ "$GEM" = 1 ] || printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
	/dev/dsk/c0d0s3 /dev/rdsk/c0d0s3 /home ufs 2 yes - >> "$W/src/vfstab"

DISKROOT_BUILD=$W/src/build DISKROOT_SRCDIR=$W/src DISKROOT_MANIFEST=$W/root.manifest \
	DISKROOT_SWAPPAGE=2 \
	KERNEL=$KERNEL ROOTMB=$ROOTMB sh "$DR/mkdiskroot.sh"

# AXB: loader and kernel, in 4 MB steps; /home: the rest, in 4 MB steps
AXBMB=$(( ($(wc -c < "$KERNEL") + 16 * 512 + 4194303) / 4194304 * 4 ))
HOMEMB=$(( (DISKMB - AXBMB - ROOTMB - SWAPMB - 1) / 4 * 4 ))
[ $HOMEMB -ge 4 ] || { echo "[FAIL] DISKMB too small"; exit 1; }
if [ "$GEM" = 1 ]; then
	[ $HOMEMB -le 508 ] || HOMEMB=508
	rm -f "$W/home.img"
	PATH=$PATH:/usr/sbin:/sbin mkfs.fat -A -n TOS -C "$W/home.img" $((HOMEMB * 1024)) > /dev/null
	HOMEID=BGM HOMES=s4
else
	# /home is mounted over the root's, so guest's home lives here
	G=$K/mac/ramdisk/build/core/home/guest
	[ -f "$G/.profile" ] || { echo "[FAIL] no $G/.profile (run mkroot.sh)"; exit 1; }
	printf '%s\n' 'd /lost+found 755 0 0' 'd /guest 755 100 1' \
		"f /guest/.profile 644 100 1 $G/.profile" > "$W/home.manifest"
	$PY "$DR/mkufs.py" -s $HOMEMB -t 723000000 -m /home "$W/home.manifest" "$W/home.img"
	HOMEID=AXU HOMES=s3
fi

$PY - "$DISKMB" "$AXBMB" "$W/src/build/root.img" "$SWAPMB" "$W/home.img" "$OUT.new" $HOMEID <<'PYEOF'
import os, sys, struct
diskmb, axbmb, root, swapmb, home, out, homeid = sys.argv[1:]
MB = 2048
parts = [(b'AXB', 64, int(axbmb) * MB)]
for pid, n in (b'AXR', os.path.getsize(root) // 512), (b'AXS', int(swapmb) * MB), \
		(homeid.encode(), os.path.getsize(home) // 512):
	parts.append((pid, parts[-1][1] + parts[-1][2], n))
total = int(diskmb) * MB
rs = bytearray(512)
struct.pack_into('>I', rs, 0x1C2, total)
for i, (pid, st, n) in enumerate(parts):
	struct.pack_into('>B3sII', rs, 0x1C6 + 12 * i, 1, pid, st, n)
with open(out, 'wb') as f:
	f.write(rs)
	for img, (pid, st, n) in (root, parts[1]), (home, parts[3]):
		f.seek(st * 512)
		with open(img, 'rb') as g:
			for b in iter(lambda: g.read(1 << 16), b''):
				if b.count(0) == len(b):
					f.seek(len(b), 1)	# keep the image sparse
				else:
					f.write(b)
	f.truncate(total * 512)
for pid, st, n in parts:
	print('%s %d+%d' % (pid.decode(), st, n))
PYEOF
sh "$A/mkboot.sh" "$W/boot"
$PY "$A/instboot.py" "$OUT.new" "$W/boot/bootsec.bin" "$W/boot/axbload.bin" \
	"$KERNEL" "$BOOTARGS"

echo "== checks"
$PY "$A/instboot.py" --check "$OUT.new" "$W/boot/bootsec.bin" \
	"$W/boot/axbload.bin" "$KERNEL" "$BOOTARGS"
cc -std=gnu89 -w -o "$W/ahditest" "$A/test/ahditest.c" "$A/ahdi.c"
"$W/ahditest" "$OUT.new" | tee "$W/slices.txt"
set -- $(awk '$1 == "s1" { print $2, $3 }' "$W/slices.txt")
cmp -n $(($2 * 512)) "$W/src/build/root.img" "$OUT.new" -i 0:$(($1 * 512)) ||
	{ echo "[FAIL] slice 1 is not the root image"; exit 1; }
awk -v n=$((SWAPMB * 2048)) '$1 == "s2" { if ($3 != n) exit 1; f = 1 } END { exit !f }' "$W/slices.txt" ||
	{ echo "[FAIL] no swap slice of $SWAPMB MB"; exit 1; }
set -- $(awk -v s=$HOMES '$1 == s { print $2, $3 }' "$W/slices.txt")
cmp -n $(($2 * 512)) "$W/home.img" "$OUT.new" -i 0:$(($1 * 512)) ||
	{ echo "[FAIL] slice $HOMES is not the $HOMEID image"; exit 1; }
rm -f "$W/home.img"
mv "$OUT.new" "$OUT"
echo "== done: $OUT ($(du -k "$OUT" | cut -f1) KB on disk, $(wc -c < "$OUT") bytes)"
