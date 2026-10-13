#!/bin/sh
# handoff.sh -- the Radeon test disk for a real 68060 Amiga with a PCI
# bridge: kernel, X with GLX/DRI, GL, the Amiga environment with Warp3D,
# Quake and the test programs, through mkhdf.sh.
#
#   sh kernel/amiga/test/handoff.sh [outdir]
#
# Out (default images/amiga-handoff): amix.hdf (with boot/ for an A3000)
# and TESTSHEET.md.
# Builds the Amiga kernel (KERNEL skips it), the Mac kernel if MACKERNEL
# (stage.sh's) is missing, the X server and package
# (X11W), Mesa and its DRI drivers (GLW), Quake and, where gl/quake/build.sh
# offers GL=yes, glquake (QW), and the display and environment packages.
# AMIGAENV: mkamiga.sh's outdir (default images/work/amiga-stage).
# QUAKEPAK (default media/quake/id1/pak0.pak) makes the image private:
# never publish it.  UNIXBOOT, OPENPCI: as mkhdf.sh, defaulting to
# toolchain/dl/amigapci's.  ROOTMB (default 384), AUTORUN: as mkhdf.sh.
# HOST picks the machine and the root disk's bus, all in mkhdf.sh: a3000
# (SCSI), a4000 (motherboard IDE); ROOTBUS=csppc puts the root on a
# CyberStorm PPC's SCSI instead, target CSPPCID (default 0).
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
ROOTMB=${ROOTMB:-384}
HOST=${HOST:-a3000}
export UNIXBOOT OPENPCI AMIGAENV X11W GLW QW ROOTMB HOST ROOTBUS
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
log w3dtest
run w3dtest sh "$K/guest-amiga/test/w3dtest/build.sh" "$W/w3d"
run warp3d sh "$K/guest/amiga/warp3d/build.sh" "$W/w3dlib"

{ printf '\tSystem V Release 4.0\t\tAsh Nazag\n\n'
  echo ' Card    drmtest         Voodoo3 3D device; radeontest 10: the Radeon'
  echo ' X       startx          twm; glxgears -info in an xterm draws GL gears'
  echo ' Amiga   startmig        Workbench; Shell: w3dtest (Warp3D); makeamiga once'
  echo '                         for your own SYS: in ~/Amiga'
  echo ' Quake   /usr/games/quake        software; in X:  /usr/games/glquake'
  echo '         +timedemo demo1 prints frames per second'
  echo
  echo ' Ctrl-Alt-0 shows the console, Ctrl-Alt-1 to -9 the sessions.'
  echo ' X depth: DEPTH=16 or 32 in /etc/default/x.  modadmin -s lists modules.'
  echo ' Shut down with  shutdown -y -g0 -i0'; } > "$W/motd"
{ echo "f /etc/motd 644 2 2 $W/motd"
  echo "f /amiga/sys/C/w3dtest 755 0 3 $W/w3d/w3dtest"
  echo "f /amiga/sys/LIBS/Warp3D.library 755 0 3 $W/w3dlib/Warp3D.library"; } > "$W/extra.manifest"

log disk
[ -f "${QUAKEPAK:-/nonexistent}" ] || QUAKEPAK=
PKG=$W/package X11PKG=$X11W/pkg GLBIN=$GLW/bin QUAKE=$QW/quake GLQUAKE=$GLQUAKE \
	QUAKEPAK=$QUAKEPAK EXTRA=$W/extra.manifest \
	$N sh "$T/mkhdf.sh" "$KERNEL" "$O" > "$W/mkhdf.log" 2>&1 ||
	{ tail -20 "$W/mkhdf.log"; echo "[FAIL] mkhdf (log: $W/mkhdf.log)"; exit 1; }
cp "$AUX/docs/radeon-test.md" "$O/TESTSHEET.md"
rm -rf "$O/src"
echo "[OK] $O ($HOST): amix.hdf ($(du -m "$O/amix.hdf" | cut -f1) MB), TESTSHEET.md"
