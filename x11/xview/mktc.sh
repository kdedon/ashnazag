#!/bin/sh
# mktc.sh -- a copy of the cross toolchain whose gcc also sets a0 = d0
# before every rts, and whose headers are fixed for gcc: the full SVR4
# namespace (what the native cc -Xa gives) without giving up __STDC__ = 1.
#
#   sh mktc.sh dir
#
# XView defines functions as returning Xv_opaque (an integer, in d0) and
# calls them through pointer-returning prototypes (read from a0). A
# pointer function already returns in both, so only integer and void
# functions change.
set -e
TC=$(cd "$(dirname "$0")/../../toolchain/amix" && pwd)
rm -rf "$1"; mkdir -p "$1/bin"
for d in lib include; do ln -s "$TC/$d" "$1/$d"; done
mkdir -p "$1/m68k-cbm-sysv4/sysroot/usr"
for f in "$TC"/m68k-cbm-sysv4/*; do
	[ "${f##*/}" = sysroot ] || ln -s "$f" "$1/m68k-cbm-sysv4/"
done
R=$TC/m68k-cbm-sysv4/sysroot/usr
for d in "$R"/*; do
	[ "${d##*/}" = include ] || ln -s "$d" "$1/m68k-cbm-sysv4/sysroot/usr/"
done
cp -R "$R/include" "$1/m68k-cbm-sysv4/sysroot/usr/include"
grep -rl '__STDC__ *[-=]' "$1/m68k-cbm-sysv4/sysroot/usr/include" | xargs sed -i \
	-e 's/__STDC__ - 0 == 0/!defined(__STRICT_ANSI__)/g' \
	-e 's/__STDC__ == 0/!defined(__STRICT_ANSI__)/g'
for f in "$TC"/bin/*; do ln -s "$f" "$1/bin/"; done
w=$1/bin/m68k-cbm-sysv4-gcc
rm "$w"
awk '{ print } prev == "fix_asm()" && /^\{$/ {
	print "\tperl -pi -e '\''s/^(\\s*)rts\\s*$/$1mov.l %d0,%a0\\n$1rts\\n/'\'' \"$1\""
} { prev = $0 }' "$TC/bin/m68k-cbm-sysv4-gcc" > "$w"
chmod +x "$w"
grep -q 'mov.l %d0,%a0' "$w"
