#!/bin/sh
# build.sh -- XaAES for the TOS environment: the MiNT layer XAMINT.PRG,
# XaAES and its mouse module from FreeMiNT's pinned source, and the C:
# files they need.
#
#   sh kernel/guest/tos/xaaes/build.sh outdir
#
# Out: outdir/C/AUTO/XAMINT.PRG, outdir/C/MINT/XAAES/ (XAAES.KM,
# MOOSE.ADI, resources, XAAES.CNF) and outdir/xaaes-src.tar.gz, the
# FreeMiNT source XaAES comes from.  MINT: where the cross tools go
# (outdir/mint).  XAAES=deb builds XaAES with its diagnostics.
set -e
H=$(cd "$(dirname "$0")" && pwd)
AUX=${AUX:-$(cd "$H/../../../.." && pwd)}
OUT=$1
[ -n "$OUT" ] || { echo "usage: build.sh outdir" >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
W=$OUT/work
C=$OUT/C
rm -rf "$W" "$C"
mkdir -p "$W" "$C/AUTO" "$C/MINT/XAAES"
B=$(AUX=$AUX sh "$H/../mintgcc.sh" "${MINT:-$OUT/mint}")
PATH=$B:$PATH
export PATH
SRC=$(sh "$AUX/images/fetch.sh" freemint)
git -C "$SRC" archive --prefix=freemint/ -o "$OUT/xaaes-src.tar.gz" HEAD \
	CONFIGVARS RULES PHONY COPYING COPYING.GPL sys xaaes
tar -C "$W" -xzf "$OUT/xaaes-src.tar.gz"
F=$W/freemint
M="nice -n 19 make -j1"
$M -C "$F/sys/libkern" 02060 > "$W/libkern.log" 2>&1 || { tail -5 "$W/libkern.log"; exit 1; }
XT=${XAAES:-02060}
$M -C "$F/xaaes/src.km" $XT > "$W/xaaes.log" 2>&1 || { tail -5 "$W/xaaes.log"; exit 1; }
$M -C "$F/xaaes/src.km/adi/whlmoose" 02060 > "$W/moose.log" 2>&1 || { tail -5 "$W/moose.log"; exit 1; }
# the layer: 16-bit int, MiNT's headers, its libkern
CC="nice -n 19 m68k-atari-mint-gcc -mshort -m68020-60 -O2 -fomit-frame-pointer -fno-strict-aliasing"
CC="$CC -fgnu89-inline -Wall -Werror -D__KERNEL__ -I$F/sys"
objs=
for s in "$H"/*.c "$H"/*.S; do
	o=$W/$(basename "$s" | sed 's/\.[cS]$/.o/')
	$CC -c "$s" -o "$o"
	objs="$objs $o"
done
$CC -s -nostdlib -Wl,--entry,__start -o "$C/AUTO/XAMINT.PRG" $objs "$F/sys/libkern/libkern02060.a" -lgcc
X=$C/MINT/XAAES
S=$F/xaaes/src.km
cp "$S/.compile_$XT"/*.km "$X/XAAES.KM"
cp "$S/adi/whlmoose/.compile_02060/moose_w.adi" "$X/MOOSE_W.ADI"
cp "$S/xaaes.rsc" "$S/xaaeswdg.rsc" "$S/xa_xtobj.rsc" "$S/xa_help.txt" "$X/"
cp -r "$S/img" "$S/pal" "$X/"
sed 's/$/\r/' "$H/xaaes.cnf" > "$X/XAAES.CNF"
rm -rf "$W"
echo "[ok] XaAES"
