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
# ZONE: /etc/TIMEZONE's TZ (default CST6CDT); not TZ, which the host's shell sets.
# BOOTARGS: kernel command line (default root=c?d0s1, ? = boot unit).
# SVIDEL=0: the loader without the SuperVidel probe, for a kernel built so.
# TESTS: programs from tests/build/bin to put in /tests, with /tests/ksyms
# (e.g. TESTS=t_page).  MODS: a module directory (guest/build.sh -m's
# mod.d) to put in /tests/mod.d, with /tests/auxreg, /dev/uinter0 and
# /dev/tos.  TOSOUT: mktos.sh's outdir; starttos and the cartridge go in /tests.
# X11: x11/package.sh's pkg directory, or 1 for the Atari X work tree
# (images/work/x11-atari), built or refreshed here; X goes on the root with the AMIX
# clients (tape segments 13, 14) and the server options in etc/xoptions.
# BOOTX=1 boots to xdm.  XPKGS="xview": packages built next to X11 (x11/NAME/build.sh).
# TOSENV=1: the TOS environment (images/tosenv/mktos.sh) with starttos,
# and the guest modules built for this kernel, registered at boot; with
# DRI's release (CPMZIP), CP/M-68K too (images/cpmenv/mkcpm.sh).
# MACENV=1: System 6 (A/UX 2.0.1's startmac, System 6.0.7, from CD201,
# default media/AUX_2.0.1_CD_Image.iso) in its own root /a201 on the ROM
# file ROM6 (default the IIci's), with startmac6 and the guest modules.
# With X11, TOSENV or MACENV, guest is in group display.
# /etc/motd lists what the image holds and how to start it.
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
ZONE=${ZONE:-CST6CDT}
PY="nice -n 19 python3"

case $KERNEL in /*) ;; *) KERNEL=$PWD/$KERNEL ;; esac
mkdir -p "$W/src/build"
ln -sfn "$DR/etc" "$W/src/etc"
ln -sfn "$A/etc" "$W/src/atari"
[ -d "$W/src/build/tape" ] || [ ! -d "$DR/build/tape" ] ||
	cp -r "$DR/build/tape" "$W/src/build/tape"
(. "$K/../toolchain/src/gcc-cross-amix/build/env.sh"
 m68k-cbm-sysv4-gcc -O -o "$W/src/build/setclk" "$A/setclk.c")
nice -n 19 sh "$K/mac/display/build.sh" "$W/display" > "$W/display.log"
# the optional-hardware drivers for this kernel; the sound service
nice -n 19 sh "$A/mods.sh" "$KERNEL" "$W/src/build/mods" > "$W/mods.log"
nice -n 19 sh "$K/mac/sound/build.sh" "$W/src/build/sound" > "$W/sound.log"
nice -n 19 sh "$K/net/build.sh" "$W/src/build/net" > "$W/net.log"
# sndaux relays the Mac environment's sound only
if [ "$MACENV" = 1 ]; then cp "$DR/etc/inittab" "$W/src/build/inittab"
else grep -v '^sa:' "$DR/etc/inittab" > "$W/src/build/inittab"; fi
# the display test built here; Atari node name; the RTC through /dev/clock
printf 'TZ=%s\nexport TZ\n' "$ZONE" > "$W/src/build/TIMEZONE"
{ grep -v -e '^f /usr/bin/dstest' -e '^r /dev/clock$' -e '^f /etc/TIMEZONE' -e '^f /etc/conf/mod.d/' -e '^f /sbin/modadmin' \
	-e '^c /dev/asc' -e '^f /usr/lib/snd' -e '^f /usr/lib/pingd' -e '^f /etc/inittab' -e '^f /etc/motd' -e '^f /etc/inet/network-config' "$DR/root.manifest"
  echo 'f /etc/motd 644 2 2 build/motd.atari'
  echo 'f /usr/sbin/bootline 744 0 3 atari/bootline'
  echo 'f /etc/TIMEZONE	444 0 3 build/TIMEZONE'
  echo "f /usr/bin/dstest	755 2 2 $W/display/dstest"
  echo 'f /etc/nodename	644 0 3 atari/nodename'
  echo 'f /etc/sysinit	744 0 3 atari/sysinit'
  echo 'f /usr/amiga/bin/setclk 755 0 3 build/setclk'
  echo 'd /home 755 0 3'
  echo 'c /dev/dpn0 600 0 3 58 0'
  echo 'f /sbin/modadmin 555 0 3 build/mods/modadmin'
  echo 'd /etc/conf 755 0 3'
  echo 'd /etc/conf/mod.d 755 0 3'
  for m in scsi sd aen dpn dmasnd auxsnd; do echo "f /etc/conf/mod.d/$m 644 0 3 build/mods/mod.d/$m"; done
  # SCSI ID n is controller 8+n: minor slice<<4 | 8 | n
  for n in 0 1 2 3 4 5 6; do for sl in 0 1 2 3 4 5 6 7; do
	echo "b /dev/dsk/c$((8 + n))d0s$sl 600 0 3 18 $((sl * 16 + 8 + n))"
	echo "c /dev/rdsk/c$((8 + n))d0s$sl 600 0 3 40 $((sl * 16 + 8 + n))"
  done; done
  echo 'c /dev/dmasnd 600 0 3 46 0'
  echo 'f /usr/lib/sndd 755 0 3 build/sound/sndd'
  echo 'f /usr/lib/sndaux 755 0 3 build/sound/sndaux'
  echo 'f /usr/lib/pingd 755 0 3 build/net/pingd'
  echo 'f /usr/bin/sndtest 755 2 2 build/sound/sndtest'
  echo 'f /etc/inittab 644 0 3 build/inittab'
  echo 'f /etc/inet/network-config 644 0 3 atari/network-config'
  echo 'f /etc/vfstab 744 0 3 vfstab'; } > "$W/root.manifest"
if [ "$X11" = 1 ]; then
	X11W=${X11W:-$(cd "$K/.." && pwd)/images/work/x11-atari}
	[ -f "$X11W/src/xc/programs/Xserver/Xamix" ] || PLATFORM=atari X11W=$X11W sh "$K/../x11/build.sh"
	PLATFORM=atari X11W=$X11W sh "$K/../x11/build.sh" clibs clients
	for p in $XPKGS; do
		[ -f "$X11W/$p/$(echo "$p" | tr a-z A-Z).pkg" ] ||
			PLATFORM=atari X11W=$X11W sh "$K/../x11/$p/build.sh"
	done
	PLATFORM=atari X11W=$X11W sh "$K/../x11/package.sh"
	X11=$X11W/pkg
fi
if [ -n "$X11" ]; then
	ln -sfn "$(cd "$X11" && pwd)" "$W/src/x11pkg"
	mkdir -p "$W/src/build/tape"
	for s in 13 14; do
		[ -f "$W/src/build/tape/$s" ] ||
			cp "${AMIX_TAPE:?AMIX_TAPE: tape segments 13 and 14}/$s" "$W/src/build/tape/$s"
	done
	{ echo 'a build/tape/13'
	  echo 'a build/tape/14'
	  for f in bin/X bin/X2410 bin/Xdmi bin/loadcoff lib/tigagm.coff; do echo "r /usr/X/$f"; done
	  echo 'r /usr/lib/dmiexec'
	  cat "$X11/x11.manifest"
	  echo 'f /usr/x11r6/lib/X11/xserver/options 644 0 3 atari/xoptions'
	  echo 'f /usr/x11r6/lib/X11/xdm/Xserver 755 0 3 atari/xserver'
	  echo 'f /usr/x11r6/lib/X11/xdm/Xservers 644 0 3 build/Xservers'; } >> "$W/root.manifest"
	# xdm's server takes the options file too
	sed 's, /usr/x11r6/bin/Xamix , /usr/x11r6/lib/X11/xdm/Xserver ,' "$X11/xdm/Xservers" > "$W/src/build/Xservers"
	grep -q xdm/Xserver "$W/src/build/Xservers"
	if [ "$BOOTX" = 1 ]; then
		sed 's/^BOOT=.*/BOOT=xdm/' "$X11/xdm/default-x" > "$W/src/build/default-x"
		echo 'f /etc/default/x 644 0 3 build/default-x' >> "$W/root.manifest"
	fi
	pk=
	for p in $XPKGS; do
		f=$X11/../$p/$(echo "$p" | tr a-z A-Z).pkg
		[ -f "$f" ] || { echo "[FAIL] no $f (x11/$p/build.sh)"; exit 1; }
		pk="$pk $f"
	done
	[ -z "$pk" ] || $PY "$DR/pkg/pkginst.py" "$W/xpkgs" $pk >> "$W/root.manifest"
fi
E=$W/env
if [ -n "$TOSENV$MACENV" ]; then
	rm -rf "$E"
	mkdir -p "$E"
	sh "$K/guest/build.sh" -m "$KERNEL" "$E/guest" > "$E/guest.log" 2>&1 ||
		{ tail "$E/guest.log"; exit 1; }
	{ echo 'd /usr/aux 755 0 3'
	  echo 'd /usr/aux/lib 755 0 3'
	  echo 'd /usr/aux/lib/mod.d 755 0 3'
	  for m in guestcore auxcore auxexec uinter tosguest; do
		echo "f /usr/aux/lib/mod.d/$m 644 0 3 $E/guest/mod.d/$m"
	  done
	  echo 'f /usr/aux/lib/auxreg 755 0 3 build/auxreg'
	  echo 'f /etc/rc2.d/S05aux 744 0 3 atari/S05aux'
	  echo 'c /dev/uinter0 660 0 25 54 0'
	  echo 'c /dev/tos 660 0 25 56 0'; } >> "$W/root.manifest"
fi
if [ "$TOSENV" = 1 ]; then
	sh "$K/../images/tosenv/mktos.sh" "$E/tos" > "$E/tos.log" 2>&1 ||
		{ tail "$E/tos.log"; exit 1; }
	sh "$K/../images/cpmenv/mkcpm.sh" "$E/cpm" > "$E/cpm.log" 2>&1 ||
		{ tail "$E/cpm.log"; exit 1; }
	sh "$K/../images/winenv/mkwin.sh" "$E/win" > "$E/win.log" 2>&1 ||
		{ tail "$E/win.log"; exit 1; }
	# guest's folders go on /home, which is mounted over the root's
	mkdir -p "$E/gt"
	for a in tos cpm win; do (cd "$E/gt" && cpio -id --quiet < "$E/$a/guest.cpio"); done
	(cd "$E/gt/home" && find guest/TOS guest/CPM guest/WIN16 2> /dev/null |
		cpio -o -H newc -R 100:1 --quiet) > "$E/homeguest.cpio"
	rm -rf "$E/gt"
	{ echo 'd /etc/tos 755 0 3'
	  echo "f /etc/tos/emutos.img 444 0 3 $E/tos/emutos.img"
	  echo "f /etc/tos/tosml.img 444 0 3 $E/tos/tosml.img"
	  for f in starttos maketos tosdrive; do echo "f /usr/bin/$f 755 0 3 $E/tos/$f"; done
	  echo "a $E/tos/sys.cpio"
	  echo "a $E/tos/games.cpio"
	  echo "a $E/cpm/cpm.cpio"
	  echo "a $E/win/win.cpio"; } >> "$W/root.manifest"
fi
if [ "$MACENV" = 1 ]; then
	R6=$E/mac6/a201
	sh "$K/../images/macenv/mksys6.sh" "${CD201:-$K/../media/AUX_2.0.1_CD_Image.iso}" "$R6" \
		> "$E/mac6.log" 2>&1 || { tail "$E/mac6.log"; exit 1; }
	mkdir -p "$R6/dev" "$R6/etc/aux"
	cp "${ROM6:-$K/../368CADFE - Mac IIci.ROM}" "$R6/etc/aux/rom"
	# root runs the Mac: nothing in its root writable by others
	chmod -R go-w "$R6"
	chmod 1777 "$R6/tmp"
	(cd "$E/mac6" && find a201 | LC_ALL=C sort | cpio -o -H newc -R 0:3 --quiet) > "$E/a201.cpio"
	{ echo "a $E/a201.cpio"
	  echo 'c /a201/dev/uinter0 660 0 25 54 0'
	  echo 'c /a201/dev/console 620 0 7 0 0'
	  echo 'f /usr/bin/startmac6 755 0 3 atari/startmac6'; } >> "$W/root.manifest"
fi
if [ -n "$X11$TOSENV$MACENV" ]; then
	T2=${AMIX_TAPE:-$W/src/build/tape}
	{ (cd "$W/src/build" && cpio -i --quiet --to-stdout etc/group < "$T2/02") | grep -v '^display:'
	  echo 'display::25:guest'; } > "$W/src/build/group.display"
	grep -q '^other:' "$W/src/build/group.display" || { echo "[FAIL] no group file on tape 02"; exit 1; }
	echo 'f /etc/group 444 0 3 build/group.display' >> "$W/root.manifest"
fi
if [ -n "$MODS$TOSENV$MACENV" ]; then
	TC=$K/../toolchain/amix SYS=$K/../toolchain/amix/m68k-cbm-sysv4/sysroot
	for f in "$K"/dlm/libmod/*.s; do
		"$TC/bin/m68k-cbm-sysv4-as" -o "$W/src/build/lm_${f##*/}.o" "$f"
	done
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 -I"$K/dlm/include" \
		-c "$DR/pkg/auxreg.c" -o "$W/src/build/auxreg.o"
	"$TC/bin/m68k-cbm-sysv4-ld" -o "$W/src/build/auxreg" "$SYS/usr/ccs/lib/crt1.o" \
		"$SYS/usr/ccs/lib/crti.o" "$W/src/build/auxreg.o" "$W"/src/build/lm_*.o \
		"$SYS/usr/lib/libc.so.1" "$SYS/usr/ccs/lib/crtn.o"
fi
if [ -n "$TESTS" ]; then
	TB=$K/../tests/build
	"$K/../toolchain/linux/bin/m68k-linux-gnu-nm" "$KERNEL" | awk '$3 ~ /^(freemem|availrmem|availsmem|lbolt|fpu_present|anoninfo)$/ { print $3, $1 }' \
		> "$W/src/build/ksyms"
	{ echo 'd /tests 755 0 3'
	  echo 'f /tests/ksyms 444 0 3 build/ksyms'
	  for t in $TESTS; do
		[ -x "$TB/bin/$t" ] || { echo "[FAIL] no $TB/bin/$t (run tests/build.sh)" >&2; exit 1; }
		echo "f /tests/$t 755 0 3 $TB/bin/$t"
	  done
	  if [ -n "$MODS" ]; then
		echo 'd /tests/mod.d 755 0 3'
		echo 'f /tests/auxreg 755 0 3 build/auxreg'
		echo 'c /dev/uinter0 666 0 3 54 0'
		echo 'c /dev/tos 660 0 25 56 0'
		for m in "$MODS"/*; do echo "f /tests/mod.d/${m##*/} 644 0 3 $m"; done
		[ -z "$TOSOUT" ] || { echo "f /tests/starttos 755 0 3 $TOSOUT/starttos"
			echo "f /tests/tosml.img 644 0 3 $TOSOUT/tosml.img"; }
	  fi; } >> "$W/root.manifest"
fi
{ printf '\tSystem V Release 4.0\t\tAsh Nazag\n\n'
  [ "$MACENV" != 1 ] || echo ' Mac    startmac6       System 6.0.7 (root); leave with Special > Logout'
  [ "$TOSENV" != 1 ] || { echo ' TOS    starttos        EmuTOS and TeraDesk; -P this machine'"'"'s TOS, -M mono'
	cpio -it < "$E/cpm/cpm.cpio" 2> /dev/null | grep -q CPM.SYS &&
		printf '%s\n' ' CP/M   startcpm        CP/M-68K in this terminal; EXIT ends it' \
		'                        drives A: to P: are ~/CPM/A to ~/CPM/P'
	cpio -it < "$E/win/win.cpio" 2> /dev/null | grep -q startwin &&
		printf '%s\n' ' Win16  startwin        Windows 3.x programs; startwin -install your disks first'
	:; }
  [ -z "$X11" ] || echo ' X      startx          twm; or  xsession  for a menu of sessions'
  echo ' Sound  sndtest         a tone through the DMA sound'
  echo
  echo ' Ctrl-Alt-0 shows the console, Ctrl-Alt-1 to -9 the sessions.'
  echo ' Network: set ADDR in /etc/inet/network-config and reboot; NETIF=aen0'
  echo ' is the NetUSBee, NETIF=dpn0 a BlueSCSI or DaynaPORT.'
  "$K/../toolchain/linux/bin/m68k-linux-gnu-nm" "$KERNEL" | grep -q ' ata_svidel$' &&
	echo " SuperVidel mode: bootline 'root=c?d0s1 sv=1024x768x16' (8, 16, 32 bpp)"
  echo ' bootline shows or sets the kernel command line (root).'
  echo ' exit logs out; root shuts down with  shutdown -y -g0 -i0'; } > "$W/src/build/motd.atari"
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
	[ "$TOSENV" != 1 ] || echo "a $W/env/homeguest.cpio" >> "$W/home.manifest"
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
