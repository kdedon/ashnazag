#!/bin/sh
# mkhdf.sh -- an AMIX disk for an A3000 or A4000, and the AmigaDOS
# volume that boots it.
#
#   sh mkhdf.sh [kernel [outdir]]
#
# kernel: the boot artifact (default kernel/build/unix-amiga, with
# unix-amiga.elf beside it).  Out: outdir/amix.hdf (RDB, SCSI unit 6:
# UNIX_Root ufs = c6d0s1, UNIX_Swap = c6d0s2) and outdir/boot/, the
# AmigaDOS volume: S/Startup-Sequence runs unix_boot040 on the kernel.
# A login runs on the serial port.  The root holds the amilib, opci, pci
# ne, zz9000, voodoo3, tdfx, radeonds, radeon, floppy and v3bars modules (floppy loaded at boot, the first of zz9000, voodoo3 and radeonds found, order in /etc/default/display), modadmin, lspci and pci.ids, fltool, tcpcat, drmtest, drmkill, radeontest, /dev/pci, /dev/ne0, /dev/dri/card0 (tdfx) and card1 (radeon), registered at
# run level 2; ne0 comes up as 10.0.2.15 (Amiberry's user-mode network).
# AMIX_TAPE: tape segments (default media/amix-tape).  UNIXBOOT: the
# unix_boot040 executable.  OPENPCI: openpci's unpacked archive (Libs/
# openpci.library, ENVARC/PCI-Configuration).  ROOTMB (default 128).
# LOADBASE: where the loader puts the kernel (default 0x08000000, the
# A3000's accelerator RAM); /stand/unix is linked there.
# PKG: stage.sh's outdir, for the display module and the environment
# packages.  ENVPKG: images/envpkg/mkenvpkgs.sh's outdir (AMIGAKERNEL this
# kernel's .elf), whose packages are installed instead.  AMIGAENV:
# mkamiga.sh's outdir, for Kickstart, the floppies and SYS:.  X11PKG:
# x11/package.sh's pkg directory; X and the tape's clients are installed.
# GLBIN: gl/build.sh's bin directory; its dritest, glprobe, glxgears, miptest
# and swaptest go to /usr/x11r6/bin (with X11PKG).
# QUAKE: gl/quake/build.sh's quake, installed as /usr/games/quake; with
# QUAKEPAK (the user's id1/pak0.pak) the game data goes to /usr/games/id1.
# GLQUAKE: its glquake (GL=yes), installed as /usr/games/glquake.
# EXTRA: a file of manifest lines added last, over the others.
# AUTORUN: a command run as root at the end of boot, output on the
# console and in /tmp/autorun.out.
# HOST: a3000 (default; SCSI unit 6, AmigaDOS boots from outdir/boot/) or
# a4000 (IDE master, which also answers as unit 6; the AmigaDOS volume is
# a bootable FFS partition AmigaBoot after the AMIX ones, s3).
# ROOTBUS: empty (default; root on the host's own controller, c6d0) or
# csppc: a CyberStorm PPC or MK III's SCSI, target CSPPCID (default 0),
# root c<8+id>d0s1 (c8d0s1), swap s2; outdir/boot/ is made as for an a3000 and both boot volumes
# run p5cfg before the loader.
set -e
A=$(cd "$(dirname "$0")/.." && pwd)
K=$(cd "$A/.." && pwd)
AUX=$(cd "$K/.." && pwd)
DR=$K/mac/diskroot
KERNEL=${1:-$K/build/unix-amiga}
O=${2:-$K/build/amiga/hdf}
TAPE=${AMIX_TAPE:-$AUX/media/amix-tape}
ROOTMB=${ROOTMB:-128}
LOADBASE=${LOADBASE:-0x08000000}
case ${HOST:=a3000} in
a3000|a4000) ;;
*) echo "[FAIL] HOST: a3000 or a4000"; exit 1 ;;
esac
case ${ROOTBUS:=} in
'') D=c6 ;;
csppc) CSPPCID=${CSPPCID:-0}; D=c$(printf %x $((8 + CSPPCID))) ;;
*) echo "[FAIL] ROOTBUS: empty or csppc"; exit 1 ;;
esac
PY="nice -n 19 python3"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
[ -f "$KERNEL" ] && [ -f "$KERNEL.elf" ] || { echo "[FAIL] no kernel $KERNEL(.elf)"; exit 1; }
[ -f "$UNIXBOOT" ] || { echo "[FAIL] UNIXBOOT: no unix_boot040"; exit 1; }
[ -f "$OPENPCI/Libs/openpci.library" ] || { echo "[FAIL] OPENPCI: no Libs/openpci.library"; exit 1; }
mkdir -p "$O/src/tape" "$O/src/bin"
S=$O/src
SEGS="02 03 07 10"
[ -z "$X11PKG" ] || SEGS="$SEGS 13 14"
for s in $SEGS; do
	[ -f "$S/tape/$s" ] || cp "$TAPE/$s" "$S/tape/$s"
done

x() { (cd "$S" && cpio -i --quiet --to-stdout "$1" < tape/02); }
{ x etc/inittab
  echo 'ser:23:respawn:/usr/lib/saf/ttymon -g -h -p "login: " -d /dev/term/ser -l 9600'
} > "$S/inittab"
x etc/vfstab | sed -e "s,^/dev/dsk/c0d0s1[	 ].*,/dev/dsk/${D}d0s1	/dev/rdsk/${D}d0s1	/	ufs	1	no	-," > "$S/vfstab"
grep -q "^/dev/dsk/${D}d0s1.*ufs" "$S/vfstab" || { echo "[FAIL] tape vfstab changed"; exit 1; }
printf 'amiga\n' > "$S/nodename"
cat > "$S/S05dlm" <<'DLM'
/sbin/modadmin -r cdev pci 60 >/dev/null
/sbin/modadmin -r sdev ne 61 >/dev/null
/sbin/modadmin -r cdev zz9000 49 >/dev/null
CARDS="zz9000 voodoo3 radeonds"
[ -f /etc/default/display ] && . /etc/default/display
for m in $CARDS
do /sbin/modadmin -l $m >/dev/null 2>&1 && break
done
/sbin/modadmin -r cdev tdfx 63 >/dev/null
/sbin/modadmin -r cdev radeon 66 >/dev/null
/sbin/modadmin -r cdev floppy 62 >/dev/null
/sbin/modadmin -l floppy >/dev/null
DLM
cat > "$S/display" <<'DSP'
# Display cards by preference: the first found drives the screen.
CARDS="zz9000 voodoo3 radeonds"
DSP
cat > "$S/network-config" <<'NC'
/usr/sbin/slink addaen /dev/ne0 ne0 &&
	/usr/sbin/ifconfig ne0 10.0.2.15 netmask 255.255.255.0 up -trailers &&
	/usr/sbin/route add default 10.0.2.2 1 >/dev/null
NC

sh "$A/mods.sh" "$KERNEL.elf" "$S/mods" > "$S/mods.log"
DLM=$K/dlm
for f in modload moduload modpath modstat modadm getksym; do
	m68k-cbm-sysv4-as -o "$S/bin/$f.o" "$DLM/libmod/$f.s"
done
CC="nice -n 19 m68k-cbm-sysv4-gcc -O"
$CC -I"$DLM/include" -o "$S/bin/modadmin" "$DLM/modadmin.c" "$S/bin"/mod*.o "$S/bin/getksym.o"
$CC -I"$A/include" -o "$S/bin/lspci" "$A/pci/lspci.c"
$CC -I"$K/guest/include" -o "$S/bin/fltool" "$A/floppy/fltool.c"
$CC -o "$S/bin/kread" "$A/test/kread.c"
$CC -I"$K/mac/display" -I"$K/drm" -o "$S/bin/drmtest" "$A/test/drmtest.c"
$CC -I"$K/mac/display" -I"$K/drm" -o "$S/bin/radeontest" "$A/test/radeontest.c"
$CC -I"$K/mac/display" -I"$K/drm" -o "$S/bin/drmkill" "$A/test/drmkill.c"
# libc.so is an archive: the shared part and the static rest
SYS=$AMIX_SYSROOT
rm -rf "$S/libc"; mkdir -p "$S/libc"
(cd "$S/libc" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 && ar rc ../libextra.a *.o)
$CC -c -o "$S/bin/tcpcat.o" "$A/test/tcpcat.c"
m68k-cbm-sysv4-ld -o "$S/bin/tcpcat" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
	"$S/bin/tcpcat.o" -L"$SYS/usr/lib" -lsocket -lnsl "$SYS/usr/lib/libc.so.1" "$S/libextra.a" \
	$(ls "$AMIX_CROSS_PREFIX"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1) "$SYS/usr/ccs/lib/crtn.o"
cp "$OPENPCI/Libs/openpci.library" "$S/openpci.library"
CONF=
if [ -f "$OPENPCI/ENVARC/PCI-Configuration" ]; then
	cp "$OPENPCI/ENVARC/PCI-Configuration" "$S/PCI-Configuration"
	CONF="f /etc/conf/pci/PCI-Configuration 644 0 3 PCI-Configuration"
fi
# the namelist for netstat, arp and crash: symbols where the loader puts them
sed -e "s/^	\\. = 0;/	. = $LOADBASE;/" "$A/amiga.ld" > "$S/unix.ld"
m68k-elf-ld -T "$S/unix.ld" -o "$S/unix.linked" "$KERNEL"
$PY "$A/test/namelist.py" "$KERNEL" "$S/unix.linked" "$S/unix"
[ -z "$ROOTBUS" ] || $PY "$A/test/rootdev.py" "$S/unix" 1 $CSPPCID
{ x etc/group; echo "display::25:root"; } > "$S/group"
ENV=
if [ -n "$PKG" ]; then
	ENV=$(sed -e "s, etc/conf/mod.d/, $PKG/root/etc/conf/mod.d/," "$PKG/root.manifest.add")
	ENVPKG=${ENVPKG:-$PKG/envpkg}
fi
if [ -n "$ENVPKG" ]; then
	EP=$(cd "$ENVPKG/pkgs/env" && pwd)
	ENV="$ENV
`$PY "$DR/pkg/pkginst.py" "$S/pkgs" $(ls "$EP"/*.pkg | grep -v macnet)`"
fi
if [ -n "$AMIGAENV" ]; then
	AE=$(cd "$AMIGAENV/root" && pwd)
	rm -rf "$S/amigasys"
	cp -R "$AE/amiga/sys" "$S/amigasys"
	chmod -R a+rX,go-w,u+w "$S/amigasys"
	ENV="$ENV
f /etc/amiga/kicka4000.rom 640 0 25 $AE/etc/amiga/kicka4000.rom
`for f in "$AE"/amiga/media/*; do echo "f ${f#$AE} 640 0 25 $f"; done`
`printf 't\t/amiga/sys\t0\t3\t%s' "$S/amigasys"`"
fi
if [ -n "$AUTORUN" ]; then
	printf '((%s) > /tmp/autorun.out 2>&1; cat /tmp/autorun.out > /dev/console) &\n' "$AUTORUN" > "$S/S99autorun"
	ENV="$ENV
f /etc/rc2.d/S99autorun 744 0 3 S99autorun"
fi
if [ -n "$QUAKE$GLQUAKE" ]; then
	ENV="$ENV
d /usr/games 755 2 2"
	for p in quake glquake; do
		eval q=\$$(echo $p | tr a-z A-Z)
		[ -n "$q" ] || continue
		m68k-elf-strip -o "$S/bin/$p" "$q"
		ENV="$ENV
f /usr/games/$p 555 2 2 bin/$p"
	done
	[ -z "$QUAKEPAK" ] || ENV="$ENV
d /usr/games/id1 777 2 2
f /usr/games/id1/pak0.pak 444 2 2 $(cd "$(dirname "$QUAKEPAK")" && pwd)/$(basename "$QUAKEPAK")"
fi
if [ -n "$X11PKG" ]; then
	XP=$(cd "$X11PKG" && pwd)
	ENV="$ENV
a tape/13
a tape/14
`grep -v '^f /etc/inittab ' "$XP/x11.manifest" | sed -e "s, x11pkg/, $XP/,"`"
	for p in dritest glprobe glxgears miptest swaptest; do
		[ -n "$GLBIN" ] && [ -f "$GLBIN/$p" ] || continue
		m68k-elf-strip -o "$S/bin/$p" "$GLBIN/$p"
		ENV="$ENV
f /usr/x11r6/bin/$p 555 2 2 bin/$p"
	done
fi

[ -z "$EXTRA" ] || ENV="$ENV
`cat "$EXTRA"`"
cat > "$S/root.manifest" <<EOF
a tape/02
a tape/03
a tape/07
a tape/10
r /stand/*
r /usr/amiga/bin/system.setup
f /stand/unix 644 0 3 unix
f /etc/inittab 644 0 3 inittab
f /etc/vfstab 744 0 3 vfstab
f /etc/nodename 644 0 3 nodename
f /etc/group 444 0 3 group
f /etc/TIMEZONE 444 0 3 $DR/etc/TIMEZONE
f /var/sadm/install/admin/default 444 0 3 $DR/etc/admin-default
e /var/adm/utmp 644 0 2
e /var/adm/utmpx 644 0 2
f /sbin/modadmin 555 0 3 bin/modadmin
f /usr/bin/lspci 555 2 2 bin/lspci
f /usr/share/lib/pci.ids 444 2 2 $A/pci/pci.ids
f /usr/bin/fltool 555 2 2 bin/fltool
f /usr/bin/tcpcat 555 2 2 bin/tcpcat
f /usr/bin/kread 555 2 2 bin/kread
f /usr/bin/drmtest 555 2 2 bin/drmtest
f /usr/bin/radeontest 555 2 2 bin/radeontest
f /usr/bin/drmkill 555 2 2 bin/drmkill
f /etc/rc2.d/S05dlm 744 0 3 S05dlm
f /etc/default/display 644 0 3 display
f /etc/inet/network-config 444 0 3 network-config
c /dev/pci 444 0 3 60 0
c /dev/ne0 600 0 3 61 0
d /etc/conf 755 0 3
d /etc/conf/mod.d 755 0 3
f /etc/conf/mod.d/amilib 644 0 3 mods/mod.d/amilib
f /etc/conf/mod.d/opci 644 0 3 mods/mod.d/opci
f /etc/conf/mod.d/pci 644 0 3 mods/mod.d/pci
f /etc/conf/mod.d/ne 644 0 3 mods/mod.d/ne
f /etc/conf/mod.d/zz9000 644 0 3 mods/mod.d/zz9000
f /etc/conf/mod.d/voodoo3 644 0 3 mods/mod.d/voodoo3
f /etc/conf/mod.d/tdfx 644 0 3 mods/mod.d/tdfx
f /etc/conf/mod.d/radeonds 644 0 3 mods/mod.d/radeonds
f /etc/conf/mod.d/radeon 644 0 3 mods/mod.d/radeon
f /etc/conf/mod.d/floppy 644 0 3 mods/mod.d/floppy
f /etc/conf/mod.d/v3bars 644 0 3 mods/mod.d/v3bars
c /dev/floppy0 666 0 3 62 0
c /dev/floppy1 666 0 3 62 1
c /dev/floppy2 666 0 3 62 2
c /dev/floppy3 666 0 3 62 3
c /dev/zz9000 660 0 25 49 0
d /dev/dri 755 0 3
c /dev/dri/card0 660 0 25 63 0
c /dev/dri/card1 660 0 25 66 0
d /etc/conf/pci 755 0 3
f /etc/conf/pci/openpci.library 644 0 3 openpci.library
$CONF
$ENV
EOF
(cd "$DR" && $PY mkufs.py -s "$ROOTMB" -t 723000000 -r "$S" "$S/root.manifest" "$S/root.img")
$PY "$DR/ufscheck.py" "$S/root.img" -l > "$S/root.lst" || { tail -20 "$S/root.lst"; exit 1; }
mkdir -p "$S/amigaboot/S"
cp "$UNIXBOOT" "$S/amigaboot/unix_boot040"
cp "$KERNEL" "$S/amigaboot/unix"
cat > "$S/amigaboot/amxmarks.rexx" <<'REXX'
/* the kernel's boot marks in chip RAM, after a reset: rx amxmarks.rexx */
DO i = 1 TO 2
	a = WORD('00001000 001FF000', i)
	d = C2X(IMPORT(X2C(a), 28))
	SAY a SUBSTR(d, 1, 8) 'n' X2D(SUBSTR(d, 9, 8)) 'mark' SUBSTR(d, 17, 8),
	    'prev' SUBSTR(d, 25, 8) 'arg' SUBSTR(d, 33, 8),
	    'mem' X2D(SUBSTR(d, 41, 8)) SUBSTR(d, 49, 8)
END
REXX
rm -rf "$O/boot"
BOOTPART=
V=DH0
[ "$HOST" = a3000 ] || V=SYS
: > "$S/amigaboot/S/Startup-Sequence"
if [ -n "$ROOTBUS" ]; then
	$PY "$A/test/rootdev.py" "$S/amigaboot/unix" 1 $CSPPCID
	m68k-linux-gnu-as -m68020 -o "$S/p5cfg.o" "$A/csppc/p5cfg.s"
	m68k-linux-gnu-objcopy -O binary -j .text "$S/p5cfg.o" "$S/p5cfg.bin"
	$PY "$A/test/hunk.py" "$S/p5cfg.bin" "$S/amigaboot/p5cfg"
	echo "$V:p5cfg" > "$S/amigaboot/S/Startup-Sequence"
fi
printf '%s:unix_boot040 %s:unix %s\n' $V $V "$LOADBASE" >> "$S/amigaboot/S/Startup-Sequence"
[ "$HOST" = a3000 ] || BOOTPART="AmigaBoot:DOS\\1:8:@$S/amigaboot"
[ "$HOST" = a4000 ] && [ -z "$ROOTBUS" ] || cp -R "$S/amigaboot" "$O/boot"
# a new file: an emulator still running keeps writing to the old one
$PY "$A/test/rdb.py" "$O/amix.hdf.new" "UNIX_Root:UNI\\1:$ROOTMB:$S/root.img" "UNIX_Swap:resv:32" $BOOTPART
mv "$O/amix.hdf.new" "$O/amix.hdf"
rm -f "$S/root.img"
echo "[OK] $O/amix.hdf ($HOST)"
