#!/bin/sh
# mkrelease.sh -- every package of a release, built from source: the
# ports and the environment packages in one apkg repository.
#
#   sh images/mkrelease.sh outdir
#
# Out: outdir/pkgs/*.pkg, outdir/catalog and outdir/catalog.md5, for
# repo=file:///outdir or the same tree over HTTP.  Kernels as for
# mkenvpkgs.sh.
set -e
D=$(cd "$(dirname "$0")" && pwd)
OUT=$1
case $OUT in ""|/) echo "usage: mkrelease.sh outdir" >&2; exit 2 ;; esac
rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
sh "$D/ports/mkports.sh" "$OUT/w/ports"
sh "$D/envpkg/mkenvpkgs.sh" "$OUT/w/env"
mkdir "$OUT/pkgs"
mv "$OUT"/w/ports/pkgs/*.pkg "$OUT"/w/env/pkgs/env/*.pkg "$OUT/pkgs/"
{ cat "$OUT/w/ports/catalog"; sed -n 's,^\([^|]*|[^|]*|[^|]*|\)env/,\1,p' "$OUT/w/env/catalog"; } \
	> "$OUT/catalog"
rm -rf "$OUT/w"
(cd "$OUT" && md5sum catalog > catalog.md5)
echo "[ok] $OUT: $(($(wc -l < "$OUT/catalog") - 1)) packages"
