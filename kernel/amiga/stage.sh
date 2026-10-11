#!/bin/sh
# stage.sh -- the display module and the environment packages for an
# Amiga kernel.
#
#   sh amiga/stage.sh [kernel.elf [outdir]]
#
# Out: outdir/root/etc/conf/mod.d/dsuser, outdir/root.manifest.add (it and
# its devices) and outdir/envpkg, mkenvpkgs.sh's output with this kernel's
# modules in mod.amiga.  MACKERNEL's modules go in mod.d (default
# kernel/build/unix-mac.elf, else this kernel's).  Every module is checked
# against the kernel's exports.
set -e
A=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$A/.." && pwd)
KERNEL=${1:-$K/build/unix-amiga.elf}
O=${2:-$K/build/amiga/package}
MK=${MACKERNEL:-$K/build/unix-mac.elf}
[ -f "$MK" ] || MK=$KERNEL
rm -rf "$O"
mkdir -p "$O/root/etc/conf/mod.d"
O=$(cd "$O" && pwd)
sh "$K/mac/display/mods.sh" "$KERNEL" "$O/display"
cp "$O/display/mod.d/dsuser" "$O/root/etc/conf/mod.d/"
rm -rf "$O/display"
cat > "$O/root.manifest.add" <<'MANIFEST'
d /etc/conf 755 0 3
d /etc/conf/mod.d 755 0 3
f /etc/conf/mod.d/dsuser 644 0 3 etc/conf/mod.d/dsuser
c /dev/fb0 660 0 25 51 0
c /dev/kbd 660 0 25 52 0
c /dev/mouse 660 0 25 53 0
MANIFEST
AMIGAKERNEL=$KERNEL KERNEL=$MK ATARIKERNEL=/nonexistent \
	sh "$K/../images/envpkg/mkenvpkgs.sh" "$O/envpkg" > "$O/envpkg.log" 2>&1 ||
	{ tail "$O/envpkg.log"; exit 1; }
mkdir "$O/check"
python3 "$K/mac/diskroot/pkg/pkginst.py" "$O/check" "$O"/envpkg/pkgs/env/*.pkg > "$O/check.manifest"
python3 "$A/test/stage.py" "$KERNEL" "$O"
rm -rf "$O/check" "$O/check.manifest"
echo "[OK] $O: dsuser, environment packages; preserve /dev/amiga major 6"
