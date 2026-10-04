#!/bin/sh
# setup.sh -- fill toolchain/, kernel/ and the media links from your own
# media and pinned public sources.  Safe to rerun; rerun after moving the
# repository (env.sh and config.sh hold absolute paths).
#
#   sh tools/setup.sh --tape PART1 PART2 --patch FILE [options]
#
# Media (yours; linked, never copied into tracked files):
#   --tape FILE...    AMIX 2.1 tape archive (both parts), or a .tap or raw
#                     image, or a directory of segments; 02, 04, 19 needed
#   --patch FILE      AMIX 2.1 patch disk (ADF)
#   --q800-rom FILE   Quadra 800 ROM
#   --q700-rom FILE   Quadra 700 ROM (420DBFF3)
#   --aux-cd FILE     A/UX 3.1 CD (partition map and Apple driver)
#   --aux-disk FILE   A/UX 3.1 disk image (fallback for images/mkimage.sh)
#   --macos761 FILE   Mac OS 7.6.1 CD (.iso or .7z)
#   --macos81 FILE    Mac OS 8.1 CD (.iso or .7z)
#   --tos FILE        TOS 3.06 ROM zip
#   --amiga-cd FILE   AmigaOS 3.2 CD (.iso)
# Build choices:
#   --amix-cross DIR  copy an installed gcc-cross-amix prefix instead of
#                     building it
#   --qemu            build the patched QEMU into toolchain/qemu-local
#   --x11             fetch X11R6.3 and x11r6.3-amix into ref/
#   --tos-src         fetch EmuTOS and fVDI into ref/
# JOBS (default 1) is passed to make; downloads go to toolchain/dl.
set -e
AUX=$(cd "$(dirname "$0")/.." && pwd)
# make and the toolchain builds cannot handle blanks in the prefix
case $AUX in *[[:space:]]*) echo "setup: move the checkout to a path without spaces" >&2; exit 1 ;; esac
TC=$AUX/toolchain
DL=$TC/dl
J=${JOBS:-1}
N="nice -n 19"

GCA_URL=https://github.com/isoriano1968/gcc-cross-amix
GCA_PIN=206bc9589dc8b0bae1cb2deb1c5bc7e5d3d8e190
PORT_URL=https://github.com/asokero/amix-040-060-port
PORT_PIN=54fba4de3da276e5d09d5ea09e2c76a13ec20ffa
BU_V=2.43.1
BU_SUM=13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd
GCC_V=13.3.0
GCC_SUM=0845e9621c9543a13f484e94584a49ffc0129970e9914624235fc1d061a0c083
HFS_URL=https://deb.debian.org/debian/pool/main/h/hfsutils/hfsutils_3.2.6.orig.tar.gz
HFS_SUM=bc9d22d6d252b920ec9cdf18e00b7655a6189b3f34f42e58d5bb152957289840
NBSD_URL=https://archive.netbsd.org/pub/NetBSD-archive/NetBSD-10.1/source/sets/syssrc.tgz
NBSD_SUM=76a600e703d2e964753323e264d3ec07d0c6cbe134648fc8f0f13ed9faaa1be4
EMUTOS_URL=https://downloads.sourceforge.net/project/emutos/emutos/1.4/emutos-512k-1.4.zip
EMUTOS_SUM=1ef7bd25f61bcfc66d19debc2f0ebb0d5e5ba811ccd7f0fddfcb105bb72d1663
FVDI_URL=https://github.com/th-otto/fvdi.git
FVDI_PIN=f40ae23c855d0505c1eb23c3db3c37ac62f64596
X11_URL=https://www.x.org/releases/X11R6.3/tars
X11_SUMS="xc-1.tar.gz:a1324ca19ea7a46abacb5a1a72e14e944140e73449dc524403829a694ba370b8
xc-2.tar.gz:cb9e6725a6f10b345c3dea417821d79a0738c09da0a16fe6b14b2ade878dcb8d
xc-3.tar.gz:cfc344e80624c08964369af6530e83e94682465c061cb37b9b8078e0b6676891"
XAMIX_URL=https://github.com/isoriano1968/x11r6.3-amix
XAMIX_PIN=cb61a2115659cb3ae27c649439edb2ca133131b6

die() { echo "setup: $*" >&2; exit 1; }
step() { echo "== $*"; }
abs() { [ -e "$1" ] || die "$1: not found"; echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
link() {	# source, link name in the repository
	s=$(abs "$1"); mkdir -p "$(dirname "$2")"
	[ ! -e "$2" ] || [ -L "$2" ] || die "$2 exists and is not a link; move it away"
	ln -sfn "$s" "$2"; echo "   $2 -> $s"
}
fetch() {	# url sha256 [name] -> path in $DL
	f=$DL/${3:-$(basename "$1")}
	mkdir -p "$DL"
	if ! echo "$2  $f" | sha256sum -c --status 2>/dev/null; then
		curl -fsSL -m 1800 -o "$f.part" "$1" || die "download failed: $1"
		mv "$f.part" "$f"
		echo "$2  $f" | sha256sum -c --quiet || { rm -f "$f"; die "$f: sha256 mismatch"; }
	fi
	echo "$f"
}
gitpin() {	# url commit dir
	[ -d "$3/.git" ] || git clone -q "$1" "$3"
	git -C "$3" cat-file -e "$2^{commit}" 2>/dev/null || git -C "$3" fetch -q origin
	[ "$(git -C "$3" rev-parse HEAD)" = "$(git -C "$3" rev-parse "$2^{commit}")" ] ||
		git -C "$3" checkout -q --detach "$2"
}
hfspatch() {	# hfsutils source dir; applies each patch once
	for p in "$TC"/hfsutils-patches/*.patch; do
		patch -d "$1" -p1 -R -s -f --dry-run < "$p" >/dev/null 2>&1 ||
			patch -d "$1" -p1 -s < "$p" || die "$p does not apply"
	done
}
cdimage() {	# .iso or .7z, link name
	case $1 in
	*.7z)	[ -f "$2" ] && [ ! -L "$2" ] && return
		d=$AUX/media/.unpack; rm -rf "$d"; mkdir -p "$d"
		$N 7z x -bd -o"$d" "$1" >/dev/null || die "7z failed on $1"
		iso=$(find "$d" -iname '*.iso' -o -iname '*.toast' -o -iname '*.img' | head -1)
		[ -n "$iso" ] || die "$1: no CD image inside"
		rm -f "$2"; mv "$iso" "$2"; rm -rf "$d"; echo "   $2 (unpacked)" ;;
	*)	link "$1" "$2" ;;
	esac
}

TAPE= PATCHDISK= CROSS= QEMU= X11= TOSSRC=
while [ $# -gt 0 ]; do
	case $1 in
	--tape)	# one or more values, kept newline-separated
		shift
		while [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; do
			[ -e "$1" ] || die "$1: not found"
			TAPE="$TAPE$(abs "$1")
"
			shift
		done
		[ -n "$TAPE" ] || die "--tape needs a value"
		continue ;;
	--patch) PATCHDISK=$2 ;;
	--q800-rom) Q800=$2 ;;
	--q700-rom) Q700=$2 ;;
	--aux-cd) AUXCD=$2 ;;
	--aux-disk) AUXDISK=$2 ;;
	--macos761) MAC761=$2 ;;
	--macos81) MAC81=$2 ;;
	--tos) TOSZIP=$2 ;;
	--amiga-cd) AMIGACD=$2 ;;
	--amix-cross) CROSS=$2 ;;
	--qemu) QEMU=1; shift; continue ;;
	--x11) X11=1; shift; continue ;;
	--tos-src) TOSSRC=1; shift; continue ;;
	*) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2 ;;
	esac
	[ $# -ge 2 ] || die "$1 needs a value"
	shift 2
done

for t in cc make git curl cpio python3 patch bison flex m4 sha256sum; do
	command -v $t >/dev/null || die "host tool missing: $t (see BUILDING.md)"
done

step media
T=$AUX/media/amix-tape
if [ -n "$TAPE" ]; then
	step "AMIX tape segments"
	[ ! -L "$T" ] || rm "$T"
	rc=0
	(IFS='
'; set -f; nice -n 19 python3 "$AUX/tools/amixtape.py" extract "$T" $TAPE) || rc=$?
	[ $rc -le 1 ] || die "unusable tape input"
fi
[ -z "${Q800:-}" ] || link "$Q800" "$AUX/Quadra 800.ROM"
[ -z "${Q700:-}" ] || link "$Q700" "$AUX/420DBFF3 - Quadra 700&900 & PB140&170.ROM"
[ -z "${AUXCD:-}" ] || link "$AUXCD" "$AUX/media/aux-3.1.iso"
case ${AUXDISK:-} in
"") ;;
*.zip) link "$AUXDISK" "$AUX/AUX_3_1_1GB_Use_In_Shoebill.zip" ;;
*) link "$AUXDISK" "$AUX/AUX_3_1_1GB.dsk" ;;
esac
[ -z "${MAC761:-}" ] || cdimage "$(abs "$MAC761")" "$AUX/media/Mac OS 7.6.1.iso"
[ -z "${MAC81:-}" ] || cdimage "$(abs "$MAC81")" "$AUX/media/MacOS8_1.iso"
[ -z "${TOSZIP:-}" ] || link "$TOSZIP" "$AUX/tos306us-american-24-09-1991.zip"
if [ -n "${AMIGACD:-}" ]; then
	link "$AMIGACD" "$AUX/AmigaOS3.2CD.iso"
	R=$AUX/images/work/amiga-stage/media
	[ -f "$R/ROM/kicka4000.rom" ] ||
		$N python3 "$AUX/tools/amiga/media.py" extract "$AUX/AmigaOS3.2CD.iso" "$R" >/dev/null
fi

R=$TC/amix-root
if [ ! -f "$R/.done" ]; then
	step "AMIX link kit and headers from tape segments 02, 04, 19"
	for s in 02 04 19; do
		[ -f "$T/$s" ] || die "no tape segment $s in $T (pass --tape)"
	done
	rm -rf "$R"; mkdir -p "$R"
	for s in 02 04 19; do
		(cd "$R" && cpio -idmu --no-absolute-filenames --quiet 'usr/*' < "$T/$s")
	done
	[ -f "$R/usr/sys/master.d/kernel.o" ] || die "segment 19 holds no link kit"
	touch "$R/.done"
fi
# the tape's modes include unreadable files (setuid, 0700)
chmod -R u+rwX "$R"

P=$AUX/kernel/patch
if [ ! -f "$P/payload/root/usr/sys/master.d/kernel.c" ]; then
	step "patch disk"
	[ -n "$PATCHDISK" ] || die "pass --patch with the 2.1 patch disk ADF"
	chmod -R u+rwX "$P/root" "$P/payload" 2>/dev/null || true
	rm -rf "$P/root" "$P/payload"; mkdir -p "$P/root" "$P/payload/root"
	# a shell header, then a cpio archive from offset 1 KB
	dd if="$(abs "$PATCHDISK")" bs=1k skip=1 status=none |
		(cd "$P/root" && cpio -idmu --quiet) || true
	chmod -R u+rwX "$P/root"
	A=$P/root/var/patch/archive.lha
	[ -f "$A" ] || die "$PATCHDISK: no var/patch/archive.lha"
	if command -v lhasa >/dev/null; then
		(cd "$P/payload" && lhasa xq "$A")
	elif command -v 7z >/dev/null; then
		7z x -bd -y -o"$P/payload" "$A" >/dev/null
	else
		die "need lhasa or 7z for the patch archive"
	fi
	C=$(find "$P/payload" -name archive.cpio | head -1)
	(cd "$P/payload/root" && cpio -idmu --quiet < "$C")
	[ -f "$P/payload/root/usr/sys/master.d/kernel.c" ] || die "patch archive has no kernel sources"
fi

step "NetBSD 10.1 syssrc.tgz"
fetch $NBSD_URL $NBSD_SUM >/dev/null

# binutils for m68k-elf (toolchain/bin) and m68k-linux-gnu (toolchain/linux)
binutils() {	# target prefix
	"$2/bin/$1-as" --version 2>/dev/null | grep -q " $BU_V\$" && return
	step "binutils $BU_V for $1"
	f=$(fetch https://ftp.gnu.org/gnu/binutils/binutils-$BU_V.tar.xz $BU_SUM)
	W=$TC/build-binutils-$1
	[ -d "$TC/src/binutils-$BU_V" ] || { mkdir -p "$TC/src"; $N tar xJf "$f" -C "$TC/src"; }
	rm -rf "$W"; mkdir -p "$W"
	(cd "$W" && $N "$TC/src/binutils-$BU_V/configure" --target=$1 --prefix="$2" \
		--disable-nls --disable-werror --disable-gdb --disable-sim > configure.log &&
	 $N make -j"$J" > make.log 2>&1 && make install > install.log 2>&1) ||
		die "binutils for $1 failed; logs in $W"
	rm -rf "$W"
}
binutils m68k-elf "$TC"
binutils m68k-linux-gnu "$TC/linux"

L=$TC/linux/bin/m68k-linux-gnu-gcc
if ! "$L" -dumpversion 2>/dev/null | grep -qx $GCC_V; then
	step "gcc $GCC_V for m68k-linux-gnu (C only, no libc)"
	f=$(fetch https://ftp.gnu.org/gnu/gcc/gcc-$GCC_V/gcc-$GCC_V.tar.xz $GCC_SUM)
	W=$TC/build-gcc
	[ -d "$TC/src/gcc-$GCC_V" ] || { mkdir -p "$TC/src"; $N tar xJf "$f" -C "$TC/src"; }
	rm -rf "$W"; mkdir -p "$W"
	(cd "$W" && PATH=$TC/linux/bin:$PATH $N "$TC/src/gcc-$GCC_V/configure" \
		--target=m68k-linux-gnu --prefix="$TC/linux" --enable-languages=c --disable-nls \
		--without-headers --disable-shared --disable-threads --disable-libssp \
		--disable-libquadmath --disable-libgomp --disable-libatomic --disable-multilib \
		--disable-bootstrap > configure.log &&
	 PATH=$TC/linux/bin:$PATH $N make -j"$J" all-gcc all-target-libgcc > make.log 2>&1 &&
	 PATH=$TC/linux/bin:$PATH make install-gcc install-target-libgcc > install.log 2>&1) ||
		die "gcc failed; logs in $W"
	rm -rf "$W"
fi

G=$TC/src/gcc-cross-amix
gitpin $GCA_URL $GCA_PIN "$G"
GCA="PREFIX=$TC/amix AMIX_ROOT=$R"
if [ ! -x "$TC/amix/bin/m68k-cbm-sysv4-gcc" ]; then
	if [ -n "$CROSS" ]; then
		step "AMIX cross toolchain from $CROSS"
		[ -x "$CROSS/bin/m68k-cbm-sysv4-gcc" ] || die "$CROSS: no bin/m68k-cbm-sysv4-gcc"
		cp -a "$(abs "$CROSS")" "$TC/amix"
	else
		step "AMIX cross toolchain (gcc 2.7.2.3, binutils 2.8.1)"
		[ ! -d "$TC/amix" ] || chmod -R u+w "$TC/amix"
		$N make -C "$G" all $GCA MAKE_JOBS="$J" > "$TC/amix-build.log" 2>&1 ||
			die "gcc-cross-amix failed; see $TC/amix-build.log"
	fi
fi
make -s -C "$G" env $GCA >/dev/null

HFS=$TC/src/hfsutils-3.2.6
if [ ! -x "$TC/bin/hfsck" ]; then
	step "hfsutils 3.2.6"
	f=$(fetch $HFS_URL $HFS_SUM hfsutils-3.2.6.tar.gz)
	mkdir -p "$TC/src" "$TC/bin" "$TC/share/man/man1"
	[ -d "$HFS" ] || tar -xzf "$f" -C "$TC/src" 2>/dev/null
	hfspatch "$HFS"
	(cd "$HFS" && { [ -f Makefile ] || $N ./configure --prefix="$TC"; } &&
	 $N make && make install_cli BINDEST="$TC/bin" MANDEST="$TC/share/man" &&
	 cd hfsck && $N make && install -m 755 hfsck "$TC/bin/") > "$TC/hfsutils.log" 2>&1 ||
		die "hfsutils failed; see $TC/hfsutils.log"
fi

step "68040/68060 port at $PORT_PIN"
PT=$AUX/kernel/amix-040-060-port
gitpin $PORT_URL $PORT_PIN "$PT"
if ! git -C "$PT" apply -R --check "$AUX/kernel/port-local.diff" 2>/dev/null; then
	git -C "$PT" apply "$AUX/kernel/port-local.diff" || die "port-local.diff does not apply"
fi
cat > "$PT/config.sh" <<EOF
# written by tools/setup.sh
AUX="$AUX"
AMIX_BUILD="\$AUX/kernel"
AMIX_CROSS="\$AUX/toolchain/amix"
GCC_CROSS_ENV="\$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
M68K_GNU_BIN="\$AUX/toolchain/linux/bin"
# relinked from the tape's link kit by kernel/build.sh
AMIX_ROOT="\${AMIX_ROOT_OVERRIDE:-\$AUX/kernel/amix-2.1p2a}"
NETBSD_SYSSRC="\$AUX/toolchain/dl/syssrc.tgz"
TMPDIR="\$AUX/kernel/build/tmp"
mkdir -p "\$TMPDIR"
export TMPDIR
AMIBERRY_BIN="/nonexistent"; AMIBERRY_CONF="/nonexistent"; AMIBERRY_HDF="/nonexistent"
XSVGA_EXP="/nonexistent"; VA2000_SRC="/nonexistent"
Z3660_SCSI_SRC="/nonexistent"; Z3660_NET_SRC="/nonexistent"
EOF

if [ -n "$TOSSRC" ]; then
	step "EmuTOS 1.4 and fVDI"
	f=$(fetch $EMUTOS_URL $EMUTOS_SUM)
	mkdir -p "$AUX/ref/emutos-release"
	cp -p "$f" "$AUX/ref/emutos-release/"
	gitpin $FVDI_URL $FVDI_PIN "$AUX/ref/fvdi"
fi
if [ -n "$X11" ]; then
	step "X11R6.3 and x11r6.3-amix"
	mkdir -p "$AUX/ref/x11r6.3/dist"
	echo "$X11_SUMS" | while IFS=: read -r n s; do
		cp -p "$(fetch $X11_URL/$n $s)" "$AUX/ref/x11r6.3/dist/$n"
	done
	gitpin $XAMIX_URL $XAMIX_PIN "$AUX/ref/x11r6.3-amix"
fi
if [ -n "$QEMU" ] && [ ! -x "$TC/qemu-local/usr/bin/qemu-system-m68k" ]; then
	step "QEMU 8.2.2 with the 68040 fix"
	sh "$TC/build-qemu.sh" > "$TC/qemu-build.log" 2>&1 || die "QEMU build failed; see $TC/qemu-build.log"
	rm -rf "$TC/qemu-build"
fi

echo "== done; now: sh tools/check-env.sh"
