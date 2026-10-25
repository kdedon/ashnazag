#!/bin/sh
# build-amix-cross.sh -- gcc 2.7.2.3 and binutils 2.8.1 for m68k-cbm-sysv4
# into toolchain/amix, from the pinned gcc-cross-amix sources, the 68060
# multiply rewrite and scanner picked from a fork, and gcc-cross-amix-patches/.
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
GCA_PIN=63d18ad981a71d14be200798b3ff6752686eb3ec
PICK_URL=https://github.com/jusii/gcc-cross-amix
PICKS="2ab2b2235f0e2d7320ba06fca0572039d3160982 80b6d678f9c408fb3e3e37b819514079c5b95ec7"
GCC_V=2.7.2.3
G=$TC/src/gcc-cross-amix
R=$1

[ -d "$G/.git" ] || git clone -q $GCA_URL "$G"
git -C "$G" cat-file -e "$GCA_PIN^{commit}" 2>/dev/null || git -C "$G" fetch -q origin
for c in $PICKS; do
	git -C "$G" cat-file -e "$c^{commit}" 2>/dev/null || git -C "$G" fetch -q $PICK_URL main
done
# the build trees are ignored files, which reset keeps
git -C "$G" reset -q --hard $GCA_PIN
git -C "$G" -c user.name=build -c user.email=build@localhost cherry-pick -n $PICKS > /dev/null
for p in "$TC"/gcc-cross-amix-patches/*.patch; do git -C "$G" apply "$p"; done
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
nice -n 19 make -C "$G" extract > "$TC/amix-build.log" 2>&1 || { tail -20 "$TC/amix-build.log"; exit 1; }
sh "$TC/fix-ashrdi16.sh" "$G/src/gcc-$GCC_V/config/m68k/m68k.md"
nice -n 19 make -C "$G" $T PREFIX="$TC/amix" AMIX_ROOT="$R" MAKE_JOBS="${JOBS:-1}" \
	>> "$TC/amix-build.log" 2>&1 || { tail -20 "$TC/amix-build.log"; exit 1; }
