#!/bin/sh
# handoff.sh -- the Radeon test disk for a real 68060 Amiga with a PCI
# bridge: kernel, X with GLX/DRI, GL, the Mac, TOS and Amiga environments,
# Quake and the test programs, through mkhdf.sh.
#
#   sh kernel/amiga/test/handoff.sh [outdir]
#
# Out (default images/amiga-handoff): amix.hdf (the last one kept as
# amix.hdf.prev), TESTSHEET.md, amix-hdf.zip of both and parts/, the zip
# split for mail with JOIN.txt.  The build ID (the kernel's, as an 060
# shows it) heads the motd and the sheet.
# Builds the Amiga kernel (KERNEL skips it), the Mac kernel if MACKERNEL
# (stage.sh's) is missing, the X server and package
# (X11W), Mesa and its DRI drivers (GLW), Quake and, where gl/quake/build.sh
# offers GL=yes, glquake (QW), and the display and environment packages.
# AMIGAENV: mkamiga.sh's outdir (default images/work/amiga-stage).
# QUAKEPAK (default media/quake/id1/pak0.pak) makes the image private:
# never publish it.  Mac: A/UX's Mac files (AUXROOT, default the path in
# tests/aux/auxroot), System 7.6.1 from CD761 and the ROM file MACROM
# (default the Quadra 800's).  TOS: guest's TOS folder (with XaAES) and
# CUBASE (mkcubase.sh's) as root's ~/TOS.  Amiga: BQUAKE (BlitzQuake's 68k
# archive) in SYS:Quake with QUAKEPAK.  All proprietary.  UNIXBOOT,
# OPENPCI: as mkhdf.sh, defaulting to toolchain/dl/amigapci's.  ROOTMB
# (default 352), SWAPMB (default 48; memory reservations count free RAM too),
# AUTORUN: as mkhdf.sh.
# HOST picks the machine and the root disk's bus, all in mkhdf.sh: a3000
# (SCSI), a4000 (default; motherboard IDE); ROOTBUS (default ncr770) puts
# the root on a CyberStorm PPC's SCSI, target CSPPCID (default 0); empty,
# on the host's own.
set -e
T=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$T/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
O=${1:-$AUX/images/amiga-handoff}
DL=$AUX/toolchain/dl/amigapci
UNIXBOOT=${UNIXBOOT:-$DL/amix-unix-boot/src/unix_boot040}
OPENPCI=${OPENPCI:-$DL/x-openpci}
AMIGAENV=${AMIGAENV:-$AUX/images/work/amiga-stage}
QUAKEPAK=${QUAKEPAK-$AUX/media/quake/id1/pak0.pak}
X11W=${X11W:-$AUX/images/work/x11-mac}
GLW=${GLW:-$AUX/images/work/gl}
QW=${QW:-$AUX/images/work/quake}
AUXROOT=${AUXROOT:-$(cat "$AUX/tests/aux/auxroot")}
CD761=${CD761:-$AUX/media/Mac OS 7.6.1.iso}
MACROM=${MACROM:-$AUX/Quadra 800.ROM}
CUBASE=${CUBASE:-$AUX/images/work/cubase/cubase.cpio}
BQUAKE=${BQUAKE:-$AUX/media/amiga/BlitzQuake_68k.lha}
ROOTMB=${ROOTMB:-352}
SWAPMB=${SWAPMB:-48}
HOST=${HOST:-a4000}
ROOTBUS=${ROOTBUS-ncr770}
for f in "$AUXROOT/shlib/libmac1_s" "$CD761" "$MACROM" "$CUBASE" "$BQUAKE" "$QUAKEPAK"; do
	[ -f "$f" ] || { echo "[FAIL] missing $f"; exit 1; }
done
export UNIXBOOT OPENPCI AMIGAENV X11W GLW QW ROOTMB SWAPMB HOST ROOTBUS
N="nice -n 19"
mkdir -p "$O/work"
O=$(cd "$O" && pwd)
W=$O/work
log() { echo "== $1"; }
run() { n=$1; shift; "$@" > "$W/$n.log" 2>&1 || { tail -20 "$W/$n.log"; echo "[FAIL] $n (log: $W/$n.log)"; exit 1; }; }

if [ -z "$KERNEL" ]; then
	log kernel
	(cd "$K" && PLATFORM=amiga $N sh build.sh) || exit 1
	KERNEL=$K/build/unix-amiga
fi
# the packages' Mac modules; mod.amiga holds only what differs from them
MACKERNEL=${MACKERNEL:-$K/build/unix-mac.elf}
if [ ! -f "$MACKERNEL" ]; then
	log "Mac kernel"
	(cd "$K" && $N sh build.sh) || exit 1
fi
export MACKERNEL
log X
if [ -f "$X11W/src/xc/programs/Xserver/Xamix" ]; then
	run x11 $N sh "$AUX/x11/build.sh" dri server
else
	run x11 $N sh "$AUX/x11/build.sh"
fi
run x11pkg $N sh "$AUX/x11/package.sh"
log GL
run gl $N sh "$AUX/gl/build.sh"
log packages
run stage $N sh "$K/amiga/stage.sh" "$KERNEL.elf" "$W/package"
log quake
run quake $N sh "$AUX/gl/quake/build.sh"
GLQUAKE=
if grep -q 'GL=yes' "$AUX/gl/quake/build.sh"; then
	run glquake env GL=yes $N sh "$AUX/gl/quake/build.sh"
	GLQUAKE=$QW/glquake
fi
log environments
sh "$AUX/images/macenv/mkmacroot.sh" "$AUXROOT" "$CD761" "$W/macroot" > "$W/macroot.log" 2>&1 ||
	{ tail -20 "$W/macroot.log"; echo "[FAIL] macroot (log: $W/macroot.log)"; exit 1; }
find "$W/macroot" -type d -exec chmod 755 {} +
chmod 700 "$W/macroot/.mac"
find "$W/macroot" -type f -perm -u+x -exec chmod 755 {} +
find "$W/macroot" -type f ! -perm -u+x -exec chmod 644 {} +
cp "$MACROM" "$W/mac.rom"
rm -rf "$W/cubase" "$W/bquake"
mkdir -p "$W/cubase" "$W/bquake"
(cd "$W/cubase" && cpio -id --quiet < "$CUBASE")
7z x -y -o"$W/bquake" "$BQUAKE" > /dev/null
# BlitzQuake wants quakedir: and a big stack
cat > "$W/bquake/quake" <<'EOF'
.key ARGS/F
Assign quakedir: SYS:Quake
cd quakedir:
stack 600000
glquake68k_blitz -bpp 16 -width 640 -height 480 -mem 16 -zone 1024 -nojoy -nocdaudio -nopsx <ARGS>
EOF
log w3dtest
run w3dtest sh "$K/guest-amiga/test/w3dtest/build.sh" "$W/w3d"
run warp3d sh "$K/guest/amiga/warp3d/build.sh" "$W/w3dlib"

BUILD=$(strings "$KERNEL" | sed -n 's/^ 680[46]0-\([0-9]*-[0-9]*\)$/68060-\1/p' | head -1)
[ -n "$BUILD" ] || { echo "[FAIL] no build ID in $KERNEL"; exit 1; }
{ printf '\tSystem V Release 4.0\t\tAsh Nazag\t%s\n\n' "$BUILD"
  echo ' Card    drmtest         Voodoo3 3D device; radeontest 10: the Radeon'
  echo ' X       startx          twm; glxgears -info in an xterm draws GL gears'
  echo ' Mac     startmac        System 7.6.1; leave with the Apple menu > Log Out'
  echo ' TOS     starttos        EmuTOS desktop;  starttos -e xaaes  XaAES'
  echo '         starttos -C /TOS/CUBASE/CUBASE31    Cubase 3.1 (also SCORE, CAF)'
  echo ' Amiga   startamiga      Workbench; Shell: w3dtest (Warp3D), IBrowse in SYS:IBrowse'
  echo '         Shell: execute SYS:Quake/quake    BlitzQuake'
  echo ' Quake   /usr/games/quake        software; in X:  /usr/games/glquake'
  echo '         +timedemo demo1 prints frames per second'
  echo
  echo ' Ctrl + Left Alt + Left Amiga + 0 shows the console, 1 to 9 the sessions.'
  echo ' X depth: 16 on a Voodoo3; DEPTH=8, 16 or 32 in /etc/default/x.  modadmin -s lists modules.'
  echo ' Shut down with  shutdown -y -g0 -i0'; } > "$W/motd"
# root's ~/Amiga, as startamiga makes it, so SYS: is writable
rm -rf "$W/amigasys"
cp -R "$AMIGAENV/root/amiga/sys" "$W/amigasys"
chmod -R a+rX,go-w,u+w "$W/amigasys"
sh "$K/guest/amiga/rtg/build.sh" "$W/rtg" > /dev/null
cp "$W/rtg/container.card" "$W/amigasys/LIBS/Picasso96/container.card"
sysfiles() {
  echo "f $1/C/w3dtest 755 0 3 $W/w3d/w3dtest"
  echo "f $1/LIBS/Warp3D.library 755 0 3 $W/w3dlib/Warp3D.library"
  B=$W/bquake/BlitzQuake_68k
  echo "d $1/Quake 755 0 3"
  echo "f $1/Quake/glquake68k_blitz 755 0 3 $B/glquake68k_blitz"
  echo "f $1/Quake/quake 755 0 3 $W/bquake/quake"
  printf 't\t%s/Quake/Docs\t0\t3\t%s\n' "$1" "$B/Docs"
  echo "d $1/Quake/id1 755 0 3"
  echo "f $1/Quake/id1/pak0.pak 644 0 3 $QUAKEPAK"
  echo "f $1/Quake/id1/fastrender.cfg 644 0 3 $B/Configs/fastrender.cfg"
}
{ echo "f /etc/motd 644 2 2 $W/motd"
  sysfiles /amiga/sys
  printf 't\t/Amiga\t0\t3\t%s\n' "$W/amigasys"
  sysfiles /Amiga
  echo "f /etc/aux/rom 444 0 3 $W/mac.rom"
  echo "d /shlib 755 0 3"
  echo "f /shlib/libc1_s 755 0 3 $AUXROOT/shlib/libc1_s"
  echo "f /shlib/libmac1_s 755 0 3 $AUXROOT/shlib/libmac1_s"
  echo "f /etc/aux/fidd 755 0 3 $AUXROOT/etc/fidd"
  for f in "$W/macroot"/* "$W/macroot"/.mac; do
	[ "$f" = "$W/macroot/usr" ] || printf 't\t/%s\t0\t3\t%s\n' "${f#$W/macroot/}" "$f"
  done
  echo "f /usr/bin/systemfolder 755 0 3 $W/macroot/usr/bin/systemfolder"
  echo "f /usr/lib/updtsysfldr 755 0 3 $W/macroot/usr/lib/updtsysfldr"
  printf 't\t/TOS\t0\t3\t%s\n' "$W/package/envpkg/guest/home/guest/TOS"
  printf 't\t/TOS/CUBASE\t0\t3\t%s\n' "$W/cubase/home/guest/TOS/CUBASE"; } > "$W/extra.manifest"

log disk
[ ! -f "$O/amix.hdf" ] || mv "$O/amix.hdf" "$O/amix.hdf.prev"
[ -f "${QUAKEPAK:-/nonexistent}" ] || QUAKEPAK=
PKG=$W/package X11PKG=$X11W/pkg GLBIN=$GLW/bin QUAKE=$QW/quake GLQUAKE=$GLQUAKE \
	QUAKEPAK=$QUAKEPAK EXTRA=$W/extra.manifest \
	$N sh "$T/mkhdf.sh" "$KERNEL" "$O" > "$W/mkhdf.log" 2>&1 ||
	{ tail -20 "$W/mkhdf.log"; echo "[FAIL] mkhdf (log: $W/mkhdf.log)"; exit 1; }
sed "1a\\
\\
Build $BUILD." "$AUX/docs/radeon-test.md" > "$O/TESTSHEET.md"
rm -rf "$O/src"

log zip
rm -rf "$O/parts" "$O/amix-hdf.zip"
(cd "$O" && $N zip -q -9 amix-hdf.zip amix.hdf TESTSHEET.md)
mkdir "$O/parts"
(cd "$O/parts" && split -b 19922944 -d -a 2 ../amix-hdf.zip amix-hdf.zip. &&
	sha256sum amix-hdf.zip.?? > parts.sha256)
cp "$O/TESTSHEET.md" "$O/parts/"
n=$(ls "$O"/parts/amix-hdf.zip.?? | wc -l)
last=$(printf '%02d' $((n - 1)))
plus=$(cd "$O/parts" && ls amix-hdf.zip.?? | paste -sd+)
{ echo "amix-hdf.zip in $n pieces (amix-hdf.zip.00 to .$last). Put them in one folder, join them in order, then unzip."
  echo
  echo "Linux / macOS:"
  echo "  cat amix-hdf.zip.?? > amix-hdf.zip"
  echo "  sha256sum amix-hdf.zip        (macOS: shasum -a 256 amix-hdf.zip)"
  echo "  unzip amix-hdf.zip"
  echo
  echo "Windows (Command Prompt):"
  echo "  copy /b $plus amix-hdf.zip"
  echo "  certutil -hashfile amix-hdf.zip SHA256"
  echo "  then open amix-hdf.zip and extract it"
  echo
  echo "The joined zip must give:"
  echo "  $(sha256sum < "$O/amix-hdf.zip" | cut -d' ' -f1)"
  echo
  echo "Piece checksums are in parts.sha256."
  echo
  echo "The zip holds amix.hdf (the disk image, $(du -m "$O/amix.hdf" | cut -f1) MB unpacked) and TESTSHEET.md, build $BUILD."
  echo "amix.hdf must give sha256 $(sha256sum < "$O/amix.hdf" | cut -d' ' -f1)."
  echo "Then make amix.hdf the CyberStorm SCSI ID 0 disk (see TESTSHEET.md)."; } > "$O/parts/JOIN.txt"
rm -rf "$W"
echo "[OK] $O ($HOST, $BUILD): amix.hdf ($(du -m "$O/amix.hdf" | cut -f1) MB), amix-hdf.zip, $n parts"
