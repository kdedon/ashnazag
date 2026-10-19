#!/bin/sh
# build-gcc295.sh -- gcc 2.95.3 (C only) for m68k-cbm-sysv4 into amix-gcc295/.
#
#   sh toolchain/build-gcc295.sh [WORK]
#   sh toolchain/build-gcc295.sh --wrapper	(rewrite the driver wrapper only)
#
# Needs toolchain/amix and toolchain/src/gcc-cross-amix from setup.sh: the
# binutils 2.8.1 and the sysroot are shared with it, and its source patches
# and assembler-fixup wrapper are reused, since both compilers emit the same
# SGS-syntax assembly.  WORK (default toolchain/gcc295-build) holds the
# source and build trees and is deleted on success.
set -e
TC=$(cd "$(dirname "$0")" && pwd)
W=${1:-$TC/gcc295-build}
DEST=$TC/amix-gcc295
OLD=$TC/amix
GCA=$TC/src/gcc-cross-amix
T=m68k-cbm-sysv4
V=2.95.3
SUM=56811ee60990b4660d54c2a66f5e0253a487e330ee53b499d8a565882ff16ef7
F="-O2 -std=gnu89 -fcommon -no-pie -w"
LIBDIR=$DEST/lib/gcc-lib/$T/$V
# freestanding libgcc2 routines, built one object per function
FUNCS="_muldi3 _divdi3 _moddi3 _udivdi3 _umoddi3 _negdi2 _lshrdi3 _ashldi3
	_ashrdi3 _ffsdi2 _udiv_w_sdiv _udivmoddi4 _cmpdi2 _ucmpdi2 _floatdidf
	_floatdisf _floatdixf _fixunsdfsi _fixunssfsi _fixunsdfdi _fixdfdi
	_fixunssfdi _fixsfdi __gcc_bcmp _clear_cache _shtab _trampoline"

# the wrapper finds cc1 through GCC_EXEC_PREFIX, so the tree can move.
# -fPIC names constant labels LC%N, which GNU as reads as an operator, and
# its PLT calls need 32-bit displacements in objects over 32 KB
wrapper() {
	sed -e "s/@TARGET@/$T/g" \
		-e '/^prefix=/a export GCC_EXEC_PREFIX="$prefix/lib/gcc-lib/"' \
		-e '/^fix_asm()/{n;a\
	perl -pi -e '"'"'s/\\bLC%(\\d+)/.LCP$1/g; s/^(\\s*b(?:sr|ra))(\\s+\\S+\\@PLTPC)/$1.l$2/'"'"' "$1"
}' \
		"$GCA/amix-gcc-wrapper.sh" > "$DEST/bin/$T-gcc"
	chmod 755 "$DEST/bin/$T-gcc"
}
[ "$1" != --wrapper ] || { wrapper; exit 0; }

[ -x "$OLD/bin/$T-as" ] || { echo "$OLD: run tools/setup.sh first" >&2; exit 1; }
mkdir -p "$W" "$TC/dl"
W=$(cd "$W" && pwd)
N="nice -n 19"

tgz=$TC/dl/gcc-core-$V.tar.gz
[ -f "$tgz" ] || curl -fsSL -o "$tgz" https://ftp.gnu.org/gnu/gcc/gcc-$V/gcc-core-$V.tar.gz
echo "$SUM  $tgz" | sha256sum -c -
rm -rf "$W/gcc-$V" "$W/build"
(cd "$W" && $N tar xzf "$tgz")
S=$W/gcc-$V
make -s -C "$GCA" refresh-config TREE="$S"
make -s -C "$GCA" patch-gcc TREE="$S/gcc"
# cc1 rejects -W[no-]return-type: c-decl.c handles it, the option table misses it
sed -i 's/^  { "-Wno-main", "" },$/&\n  { "-Wreturn-type", "" },\n  { "-Wno-return-type", "" },/' \
	"$S/gcc/toplev.c"
# -fPIC: the frame layout counts the GOT register in any function that loads
# it, but its save, restore and the bare-rts return went by whether reload
# used it; such functions read their arguments 4 bytes off and clobber it
M68K=$S/gcc/config/m68k/m68k.c
n=$(grep -c current_function_uses_pic_offset_table "$M68K" || true)
sed -i -e 's/flag_pic && regs_ever_live\[PIC_OFFSET_TABLE_REGNUM\]/flag_pic \&\& current_function_uses_pic_offset_table/' \
	-e 's/get_frame_size () != 0)$/get_frame_size () != 0\n      || (flag_pic \&\& current_function_uses_pic_offset_table))/' \
	"$M68K"
[ "$(grep -c current_function_uses_pic_offset_table "$M68K")" -eq $((n + 3)) ] ||
	{ echo "$M68K: the -fPIC fix did not apply" >&2; exit 1; }

mkdir -p "$W/build"
cd "$W/build"
CC=gcc CFLAGS="$F" $N "$S/configure" --host=i686-pc-linux-gnu \
	--build=i686-pc-linux-gnu --target=$T --prefix="$DEST" \
	--with-gnu-as --with-gnu-ld --enable-languages=c > configure.log
$N make -j1 CFLAGS="$F" MAKEINFO=true all-libiberty > make.log
PATH=$OLD/bin:$PATH $N make -j1 -C gcc LANGUAGES=c CFLAGS="$F" MAKEINFO=true \
	xgcc cc1 cccp >> make.log

rm -rf "$DEST"
mkdir -p "$DEST/bin" "$DEST/$T" "$LIBDIR"
for f in "$OLD"/bin/$T-*; do
	case $f in *-gcc|*-gcc.real|*-ar|*-ranlib) ;; *) cp -p "$f" "$DEST/bin/" ;; esac
done
# 2.8.1 ar aborts under a fortified host libc; use the host ar
make -s -C "$GCA" install-target-ar PREFIX="$DEST" TARGET=$T
ln -s ../../amix/$T/sysroot "$DEST/$T/sysroot"
cp gcc/xgcc "$DEST/bin/$T-gcc.real"
cp gcc/cc1 "$LIBDIR/cc1"
cp gcc/cccp "$LIBDIR/cpp0"
gcc/xgcc -B gcc/ -dumpspecs > "$LIBDIR/specs"
ln -s ../../../../bin/$T-as "$LIBDIR/as"
ln -s ../../../../bin/$T-ld "$LIBDIR/ld"
wrapper

mkdir -p libgcc
# a compile-only toolchain has no AMIX headers
NOLIBC=
if [ ! -f "$OLD/$T/sysroot/usr/include/stdlib.h" ]; then
	mkdir -p nolibc
	cp "$S/gcc/glimits.h" nolibc/limits.h
	NOLIBC="-Dinhibit_libc -I nolibc -I $S/gcc/ginclude"
fi
for f in $FUNCS; do
	$N "$DEST/bin/$T-gcc" -O2 -DIN_GCC -DCROSS_COMPILE -DIN_LIBGCC2 $NOLIBC \
		-D__GCC_FLOAT_NOT_NEEDED -I gcc -I gcc/include -I "$S/gcc" \
		-I "$S/gcc/config" -I "$S/include" \
		-DL$f -c "$S/gcc/libgcc2.c" -o libgcc/$f.o
done
"$DEST/bin/$T-ar" rc "$LIBDIR/libgcc.a" libgcc/*.o
"$DEST/bin/$T-ranlib" "$LIBDIR/libgcc.a"

cd "$TC"
rm -rf "$W"
"$DEST/bin/$T-gcc.real" -v 2>&1 | tail -1
