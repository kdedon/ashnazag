#!/bin/sh
# Fetch, configure and build Hatari for the Atari port tests.
#
#   build-hatari.sh [-n]     -n: configure only
#
# Does nothing while the binary is newer than every local patch and the
# source is at the pinned revision.
# Source in ref/hatari (pinned below), binary in ref/hatari/build/src/hatari.
# cmake and the SDL2 headers are fetched into ref/hatari/build/deps when the
# host lacks them; the binary links against the host's libSDL2-2.0.so.0.
set -e

URL=https://github.com/slaapliedje/hatari
REV=a88efcfc			# branch debugger-memdump-mmu
CMAKE=3.30.5

AUX=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$AUX/ref"
# one build at a time: test runs call this too
[ -n "$HATARI_BLOCK" ] || HATARI_BLOCK=1 exec flock "$AUX/ref/.hatari.lock" sh "$0" "$@"
SRC=$AUX/ref/hatari
B=$SRC/build
D=$B/deps
[ -d "$SRC/.git" ] || git clone -q "$URL" "$SRC"
# local fixes, every hatari-*.patch here: cycle-exact 030 + MMU consumed a
# faulted prefetch word; a command fifo with an idle writer was reported as
# a read error; rte reran a data cycle the bus error handler had completed
# (DF cleared).  A patch newer than the binary resets the source and
# applies them all again.
PATCHES=$(ls "$AUX"/kernel/atari/hatari-*.patch)
stale=
[ -x "$B/src/hatari" ] || stale=1
for P in $PATCHES; do
	[ "$P" -nt "$B/src/hatari" ] && stale=1
done
[ "$(git -C "$SRC" rev-parse --short=8 HEAD)" = "$REV" ] || stale=1
if [ -z "$stale" ] && [ "$1" != -n ]; then
	ls -l "$B/src/hatari"
	exit 0
fi
git -C "$SRC" cat-file -e "$REV^{commit}" 2>/dev/null || git -C "$SRC" fetch -q origin
git -C "$SRC" checkout -q -f "$REV"
git -C "$SRC" checkout -q -- .
for P in $PATCHES; do
	git -C "$SRC" apply "$P"
done
mkdir -p "$D"

CM=$(command -v cmake || true)
if [ -z "$CM" ]; then
	CM=$D/cmake-$CMAKE-linux-x86_64/bin/cmake
	[ -x "$CM" ] || curl -sSL "https://github.com/Kitware/CMake/releases/download/v$CMAKE/cmake-$CMAKE-linux-x86_64.tar.gz" | tar xz -C "$D"
fi

EXTRA=
if ! pkg-config --exists sdl2 2>/dev/null; then
	S=$D/sdl/usr
	L=$S/lib/x86_64-linux-gnu
	if [ ! -d "$S" ]; then
		(cd "$D" && apt-get download libsdl2-dev >/dev/null && dpkg-deb -x libsdl2-dev_*.deb sdl)
		so=$(ls /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0.* | head -1)
		ln -sf "$so" "$L/libSDL2.so"
		ln -sf "$so" "$L/libSDL2-2.0.so"
	fi
	export PKG_CONFIG_PATH=$L/pkgconfig
	EXTRA="-DSDL2_DIR=$L/cmake/SDL2 -DCMAKE_C_FLAGS=-I$S/include/x86_64-linux-gnu"
fi

cd "$B"
nice -n 19 "$CM" .. -DENABLE_SDL3=0 -DCMAKE_BUILD_TYPE=Release $EXTRA > configure.log
[ "$1" = -n ] && exit 0
nice -n 19 make -j1 hatari > build.log 2>&1
ls -l "$B/src/hatari"
