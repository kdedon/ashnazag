#!/bin/sh
# mkhdf.sh -- an AMIX disk for an emulated A3000, and the AmigaDOS
# directory that boots it.
#
#   sh mkhdf.sh [kernel [outdir]]
#
# kernel: the boot artifact (default kernel/build/unix-amiga, with
# unix-amiga.elf beside it).  Out: outdir/amix.hdf (RDB, SCSI unit 6:
# UNIX_Root ufs = c6d0s1, UNIX_Swap = c6d0s2) and outdir/boot/, the
# AmigaDOS volume: S/Startup-Sequence runs unix_boot040 on the kernel.
# A login runs on the serial port.  The root holds the amilib, opci, pci
# ne, zz9000 and floppy modules (floppy loaded at boot, zz9000 when the card is there), modadmin, lspci, fltool, tcpcat, /dev/pci and /dev/ne0, registered at
# run level 2; ne0 comes up as 10.0.2.15 (Amiberry's user-mode network).
# AMIX_TAPE: tape segments (default media/amix-tape).  UNIXBOOT: the
# unix_boot040 executable.  OPENPCI: openpci's unpacked archive (Libs/
# openpci.library, ENVARC/PCI-Configuration).  ROOTMB (default 128).
# LOADBASE: where the loader puts the kernel (default 0x08000000, the
# A3000's accelerator RAM); /stand/unix is linked there.
# PKG: stage.sh's outdir, for the display module and the environment
# packages.  ENVPKG: images/envpkg/mkenvpkgs.sh's outdir (AMIGAKERNEL this
# kernel's .elf), whose packages are installed instead.  AMIGAENV:
# mkamiga.sh's outdir, for Kickstart, the floppies and SYS:.
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
PY="nice -n 19 python3"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
[ -f "$KERNEL" ] && [ -f "$KERNEL.elf" ] || { echo "[FAIL] no kernel $KERNEL(.elf)"; exit 1; }
[ -f "$UNIXBOOT" ] || { echo "[FAIL] UNIXBOOT: no unix_boot040"; exit 1; }
[ -f "$OPENPCI/Libs/openpci.library" ] || { echo "[FAIL] OPENPCI: no Libs/openpci.library"; exit 1; }
mkdir -p "$O/src/tape" "$O/src/bin"
S=$O/src
for s in 02 03 07 10; do
	[ -f "$S/tape/$s" ] || cp "$TAPE/$s" "$S/tape/$s"
done

x() { (cd "$S" && cpio -i --quiet --to-stdout "$1" < tape/02); }
{ x etc/inittab
  echo 'ser:23:respawn:/usr/lib/saf/ttymon -g -h -p "login: " -d /dev/term/ser -l 9600'
} > "$S/inittab"
x etc/vfstab | sed -e 's,^/dev/dsk/c0d0s1[	 ].*,/dev/dsk/c6d0s1	/dev/rdsk/c6d0s1	/	ufs	1	no	-,' > "$S/vfstab"
grep -q '^/dev/dsk/c6d0s1.*ufs' "$S/vfstab" || { echo "[FAIL] tape vfstab changed"; exit 1; }
printf 'amiga\n' > "$S/nodename"
printf '/sbin/modadmin -r cdev pci 60 >/dev/null\n/sbin/modadmin -r sdev ne 61 >/dev/null\n/sbin/modadmin -r cdev zz9000 49 >/dev/null\n/sbin/modadmin -l zz9000 >/dev/null 2>&1\n/sbin/modadmin -r cdev floppy 62 >/dev/null\n/sbin/modadmin -l floppy >/dev/null\n' > "$S/S05dlm"
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
	A=$(cd "$AMIGAENV/root" && pwd)
	rm -rf "$S/amigasys"
	cp -R "$A/amiga/sys" "$S/amigasys"
	chmod -R a+rX,go-w,u+w "$S/amigasys"
	ENV="$ENV
f /etc/amiga/kicka4000.rom 640 0 25 $A/etc/amiga/kicka4000.rom
`for f in "$A"/amiga/media/*; do echo "f ${f#$A} 640 0 25 $f"; done`
`printf 't\t/amiga/sys\t0\t3\t%s' "$S/amigasys"`"
fi

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
f /usr/bin/fltool 555 2 2 bin/fltool
f /usr/bin/tcpcat 555 2 2 bin/tcpcat
f /usr/bin/kread 555 2 2 bin/kread
f /etc/rc2.d/S05dlm 744 0 3 S05dlm
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
f /etc/conf/mod.d/floppy 644 0 3 mods/mod.d/floppy
c /dev/floppy0 666 0 3 62 0
c /dev/floppy1 666 0 3 62 1
c /dev/floppy2 666 0 3 62 2
c /dev/floppy3 666 0 3 62 3
c /dev/zz9000 660 0 25 49 0
d /etc/conf/pci 755 0 3
f /etc/conf/pci/openpci.library 644 0 3 openpci.library
$CONF
$ENV
EOF
(cd "$DR" && $PY mkufs.py -s "$ROOTMB" -t 723000000 -r "$S" "$S/root.manifest" "$S/root.img")
$PY "$DR/ufscheck.py" "$S/root.img" -l > "$S/root.lst" || { tail -20 "$S/root.lst"; exit 1; }
# a new file: an emulator still running keeps writing to the old one
$PY "$A/test/rdb.py" "$O/amix.hdf.new" "UNIX_Root:UNI\\1:$ROOTMB:$S/root.img" "UNIX_Swap:resv:32"
mv "$O/amix.hdf.new" "$O/amix.hdf"
rm -f "$S/root.img"

mkdir -p "$O/boot/S"
cp "$UNIXBOOT" "$O/boot/unix_boot040"
cp "$KERNEL" "$O/boot/unix"
printf 'DH0:unix_boot040 DH0:unix %s\n' "$LOADBASE" > "$O/boot/S/Startup-Sequence"
echo "[OK] $O/amix.hdf, $O/boot"
