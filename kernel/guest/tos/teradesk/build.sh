#!/bin/sh
# build.sh -- TeraDesk, a replacement GEM desktop, from its pinned source.
#
#   sh kernel/guest/tos/teradesk/build.sh outdir
#
# Out: outdir/TERADESK/ (DESKTOP.PRG, its resources and licence) and
# outdir/teradesk.tar.gz, the source.  MINT: where the cross tools go
# (outdir/mint).
set -e
H=$(cd "$(dirname "$0")" && pwd)
AUX=${AUX:-$(cd "$H/../../../.." && pwd)}
OUT=$1
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
W=$OUT/work
rm -rf "$W" "$OUT/TERADESK"
mkdir -p "$W" "$OUT/TERADESK"
B=$(AUX=$AUX sh "$H/../mintgcc.sh" "${MINT:-$OUT/mint}")
SRC=$(sh "$AUX/images/fetch.sh" teradesk)
git -C "$SRC" archive --prefix=teradesk/ -o "$OUT/teradesk.tar.gz" HEAD
tar -C "$W" -xzf "$OUT/teradesk.tar.gz"
PATH=$B:$PATH nice -n 19 make -C "$W/teradesk" -j1 V=0 > "$OUT/make.log" 2>&1 ||
	{ tail -5 "$OUT/make.log"; exit 1; }
cp "$W/teradesk/desktop.prg" "$OUT/TERADESK/DESKTOP.PRG"
for f in desktop.rsc icons.rsc cicons.rsc COPYING; do
	cp "$W/teradesk/$f" "$OUT/TERADESK/$(echo $f | tr a-z A-Z)"
done
rm -rf "$W"
echo "[ok] TeraDesk"
