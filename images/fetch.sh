#!/bin/sh
# fetch.sh -- fetch a source named in vendor.manifest into the cache,
# check its pin, print its path.  Cache: $VENDOR_DL or toolchain/dl/vendor.
#
#   sh images/fetch.sh name
#
# A git source is cloned from ref/<repo> when that clone has the commit.
set -e
AUX=$(cd "$(dirname "$0")/.." && pwd)
C=${VENDOR_DL:-$AUX/toolchain/dl/vendor}
line=$(grep "^$1|" "$AUX/images/vendor.manifest") || { echo "fetch: no $1" >&2; exit 1; }
IFS='|' read -r name kind url pin lic <<EOI
$line
EOI
mkdir -p "$C"
C=$(cd "$C" && pwd)
case $kind in
tar)
	f=$C/$(basename "$url")
	if [ ! -f "$f" ]; then
		curl -fsSL -m 600 -o "$f.part" "$url" >&2
		mv "$f.part" "$f"
	fi
	echo "$pin  $f" | sha256sum -c --quiet >&2 || { rm -f "$f"; echo "fetch: $f: sha256 mismatch" >&2; exit 1; }
	echo "$f" ;;
git)
	d=$C/$name
	if [ ! -d "$d" ]; then
		src=$AUX/ref/$(basename "$url" .git)
		git -C "$src" cat-file -e "$pin^{commit}" 2>/dev/null || src=$url
		git clone -q "$src" "$d" >&2
	fi
	git -C "$d" cat-file -e "$pin^{commit}" 2>/dev/null || git -C "$d" fetch -q "$url" >&2
	[ "$(git -C "$d" rev-parse HEAD 2>/dev/null)" = "$pin" ] || git -C "$d" checkout -q --detach "$pin" >&2
	[ -z "$(git -C "$d" status --porcelain)" ] || { echo "fetch: $d: local changes" >&2; exit 1; }
	echo "$d" ;;
*)	echo "fetch: $name: kind $kind" >&2; exit 1 ;;
esac
