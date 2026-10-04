#!/bin/sh
# build-qemu.sh -- build qemu-system-m68k 8.2.2 with qemu-patches/ into qemu-local/.
#
#   sh toolchain/build-qemu.sh [WORK]
#
# WORK (default toolchain/qemu-build) holds the download, a Python venv
# with meson and ninja, and the build tree; it can be deleted afterwards.
# The install has the packaged qemu/ layout (usr/bin, usr/share/qemu, lib),
# so the launchers pick it up unchanged.  Needs cc, glib and pixman dev
# files; slirp (user networking) comes from the system or from the
# libslirp-dev package, linked statically.
set -e
TC=$(cd "$(dirname "$0")" && pwd)
W=${1:-$TC/qemu-build}
DEST=$TC/qemu-local
V=8.2.2
SUM=847346c1b82c1a54b2c38f6edbd85549edeb17430b7d4d3da12620e2962bc4f3
PIPWHL=pip-24.2-py3-none-any.whl
PIPSUM=2cd581cf58ab7fcfca4ce8efa6dcacd0de5bf8d0a3eb9ec927e07405f4d9e2a2
mkdir -p "$W"
W=$(cd "$W" && pwd)
cd "$W"

[ -f qemu-$V.tar.xz ] || curl -fsSLO https://download.qemu.org/qemu-$V.tar.xz
echo "$SUM  qemu-$V.tar.xz" | sha256sum -c -
rm -rf qemu-$V
nice -n 19 tar xJf qemu-$V.tar.xz
for p in "$TC"/qemu-patches/*.patch; do
	patch -d qemu-$V -p1 < "$p"
done

# meson and ninja in a venv; pip itself from its wheel (no ensurepip needed)
if [ ! -x venv/bin/ninja ]; then
	rm -rf venv
	python3 -m venv --without-pip --system-site-packages venv
	[ -f $PIPWHL ] || curl -fsSLO https://files.pythonhosted.org/packages/py3/p/pip/$PIPWHL
	echo "$PIPSUM  $PIPWHL" | sha256sum -c -
	venv/bin/python $PIPWHL/pip install -q --disable-pip-version-check pip==24.2
	venv/bin/pip install -q --disable-pip-version-check meson==1.2.3 ninja==1.13.0 setuptools
fi
PATH=$W/venv/bin:$PATH

if ! pkg-config --exists slirp; then
	if [ ! -f slirp/slirp.pc ]; then
		apt-get download libslirp-dev
		rm -rf slirp-deb && dpkg-deb -x libslirp-dev_*.deb slirp-deb
		mkdir -p slirp
		cp slirp-deb/usr/lib/*/libslirp.a slirp/
		cp -r slirp-deb/usr/include slirp/include
		# only the static library, so the binary needs no libslirp at run time
		sed -e "s|^prefix=.*|prefix=$W/slirp|" -e 's|^libdir=.*|libdir=${prefix}|' \
			-e 's|^includedir=.*|includedir=${prefix}/include|' \
			-e 's|^Libs:.*|Libs: -L${libdir} -lslirp|' \
			slirp-deb/usr/lib/*/pkgconfig/slirp.pc > slirp/slirp.pc
	fi
	export PKG_CONFIG_PATH=$W/slirp${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
fi

rm -rf build && mkdir build && cd build
nice -n 19 ../qemu-$V/configure --prefix="$DEST/usr" --python="$W/venv/bin/python3" \
	--target-list=m68k-softmmu --without-default-features --enable-tcg \
	--enable-slirp --enable-pixman --disable-docs --disable-tools --disable-werror
nice -n 19 ninja -j1
rm -rf "$DEST"
nice -n 19 ninja install
mkdir -p "$DEST/lib"
"$DEST/usr/bin/qemu-system-m68k" --version
