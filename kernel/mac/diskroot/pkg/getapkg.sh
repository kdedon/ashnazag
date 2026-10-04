#!/bin/sh
# getapkg.sh -- download the apkg and apkgeng datastreams from the package
# server into dir, checked against the catalog's md5 sums.
#
#   sh getapkg.sh dir
set -e
R=http://pkg.amigaux.org
mkdir -p "$1"
cd "$1"
curl -sSf -m 60 -o catalog "$R/catalog"
for n in apkgeng apkg; do
	set -- $(awk -F'|' -v n=$n '$1 == n { print $4, $5 }' catalog)
	f=$(basename "$1")
	curl -sSf -m 300 -o "$f" "$R/pkgs/$1"
	[ "$(md5sum < "$f" | cut -d' ' -f1)" = "$2" ] || { echo "[FAIL] $f: md5"; exit 1; }
	echo "[ok] $f"
done
