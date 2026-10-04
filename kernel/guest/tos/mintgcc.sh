#!/bin/sh
# mintgcc.sh -- the m68k-atari-mint cross tools (gcc 4.6, binutils,
# MiNTLib, GEMlib) from vendor.manifest, unpacked into dir.
#
#   sh kernel/guest/tos/mintgcc.sh dir
#
# Prints the directory holding m68k-atari-mint-gcc.
set -e
AUX=${AUX:-$(cd "$(dirname "$0")/../../.." && pwd)}
D=$1
mkdir -p "$D"
D=$(cd "$D" && pwd)
if [ ! -x "$D/usr/bin/m68k-atari-mint-gcc" ]; then
	rm -rf "$D/x"
	mkdir -p "$D/x"
	for p in mint-binutils mint-gcc mint-mintlib mint-gemlib; do
		f=$(sh "$AUX/images/fetch.sh" $p)
		(cd "$D/x" && rm -f data.tar.* && ar x "$f" && tar -C "$D" -xf data.tar.*)
	done
	rm -rf "$D/x"
fi
echo "$D/usr/bin"
