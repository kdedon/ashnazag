#!/bin/sh
# build-amix-cross.sh -- gcc 2.7.2.3 and binutils 2.8.1 for m68k-cbm-sysv4
# into toolchain/amix, from the pinned gcc-cross-amix sources.
#
#   sh toolchain/build-amix-cross.sh [AMIX_ROOT]
#   sh toolchain/build-amix-cross.sh -s	(sources only)
#
# With AMIX_ROOT (the AMIX headers and libraries), the full toolchain with
# its sysroot and libgcc.  Without, a compile-only one: it compiles and
# links relocatable objects but has no sysroot, libc or libgcc.
# JOBS (default 1) is passed to make.  The log is toolchain/amix-build.log.
set -e
TC=$(cd "$(dirname "$0")" && pwd)
GCA_URL=https://github.com/isoriano1968/gcc-cross-amix
GCA_PIN=206bc9589dc8b0bae1cb2deb1c5bc7e5d3d8e190
G=$TC/src/gcc-cross-amix
R=$1

[ -d "$G/.git" ] || git clone -q $GCA_URL "$G"
git -C "$G" cat-file -e "$GCA_PIN^{commit}" 2>/dev/null || git -C "$G" fetch -q origin
[ "$(git -C "$G" rev-parse HEAD)" = "$(git -C "$G" rev-parse "$GCA_PIN^{commit}")" ] ||
	git -C "$G" checkout -q --detach $GCA_PIN
[ "$R" != -s ] || exit 0
if [ -n "$R" ]; then
	T=all
else
	# the sysroot step only needs the include directory to exist
	R=$TC/amix-root-none
	mkdir -p "$R/usr/include"
	T="install-binutils install-gcc install-gcc-wrapper install-target-ar env"
fi
[ ! -d "$TC/amix" ] || chmod -R u+w "$TC/amix"
nice -n 19 make -C "$G" $T PREFIX="$TC/amix" AMIX_ROOT="$R" MAKE_JOBS="${JOBS:-1}" \
	> "$TC/amix-build.log" 2>&1 || { tail -20 "$TC/amix-build.log"; exit 1; }
