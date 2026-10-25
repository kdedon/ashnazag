#!/bin/sh
# mkports.sh -- third-party programs for /opt/amix, built from source:
# SVR4 datastreams for pkgadd or apkg on every host.
#
#   sh images/ports/mkports.sh outdir [port...]
#
#   bash      GNU bash 5.2.37
#   bzip2     bzip2 1.0.8
#   dropbear  SSH server and client (needs prngd)
#   infozip   zip 3.0, unzip 6.0
#   joe       joe 3.7 (jmacs, jpico, jstar, rjoe)
#   lynx      text web browser, no TLS
#   prngd     entropy daemon on TCP localhost:708
#   screen    GNU screen 4.0.3
#   vim       Vim 9.2 (vimdiff, rvim, rview, xxd)
#   vimdoc    its help files
#   wget      HTTP and FTP downloader, no TLS
#
# Writes outdir/pkgs/*.pkg, outdir/catalog (apkg's format, paths
# relative to outdir/pkgs) and its catalog.md5.
set -e
P=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: mkports.sh outdir [port...]}; shift
[ $# -gt 0 ] || set -- $(cd "$P" && ls */build.sh | sed 's,/build.sh,,')
for p; do
	echo "== $p"
	sh "$P/$p/build.sh" "$OUT" || exit
done
{ echo '#apkg-catalog 1'; cat "$OUT"/*.cat; } > "$OUT/catalog"
(cd "$OUT" && md5sum catalog > catalog.md5)
echo "[ok] $OUT/catalog: $(($(wc -l < "$OUT/catalog") - 1)) packages"
