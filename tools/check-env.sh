#!/bin/sh
# check-env.sh -- report what each build target still lacks.
#
#   sh tools/check-env.sh [target ...]
#
# Targets: kernel suite disk x11 mac tos amiga falcon (default: all).
# Exit status: non-zero when a named target (or, with none named, the
# kernel) is incomplete.  tools/setup.sh fills most of it in.
AUX=$(cd "$(dirname "$0")/.." && pwd)
TC=$AUX/toolchain
T=$AUX/media/amix-tape
miss=0 all=

has() {	# what, test...
	w=$1; shift
	if "$@" >/dev/null 2>&1; then :; else echo "  missing: $w"; miss=$((miss + 1)); fi
}
f() { [ -e "$1" ]; }
tool() { command -v "$1"; }

kernel() {
	for t in cc make git curl cpio python3 patch bison flex m4; do has "host tool $t" tool $t; done
	has "AMIX cross gcc (toolchain/amix)" f "$TC/amix/bin/m68k-cbm-sysv4-gcc"
	has "gcc-cross-amix env.sh" f "$TC/src/gcc-cross-amix/build/env.sh"
	has "env.sh for this checkout (rerun setup.sh)" grep -q "$TC/amix" "$TC/src/gcc-cross-amix/build/env.sh"
	has "AMIX link kit (toolchain/amix-root, tape segment 19)" f "$TC/amix-root/usr/sys/master.d/kernel.o"
	has "AMIX headers (tape segment 04)" f "$TC/amix-root/usr/include/sys/types.h"
	has "m68k-elf binutils 2.43.1" f "$TC/bin/m68k-elf-ld"
	has "m68k-linux-gnu binutils 2.43.1" f "$TC/linux/bin/m68k-linux-gnu-ld"
	has "m68k-linux-gnu-gcc 13.3.0" f "$TC/linux/bin/m68k-linux-gnu-gcc"
	has "NetBSD 10.1 syssrc.tgz" f "$TC/dl/syssrc.tgz"
	has "patch disk sources (--patch)" f "$AUX/kernel/patch/payload/root/usr/sys/master.d/kernel.c"
	has "040/060 port clone" f "$AUX/kernel/amix-040-060-port/relink-040.sh"
	has "port config.sh" f "$AUX/kernel/amix-040-060-port/config.sh"
	has "port-local.diff applied" git -C "$AUX/kernel/amix-040-060-port" apply -R --check "$AUX/kernel/port-local.diff"
}
suite() {
	kernel
	has "tape segment 02 (media/amix-tape)" f "$T/02"
	has "patched QEMU (setup.sh --qemu)" f "$TC/qemu-local/usr/bin/qemu-system-m68k"
}
disk() {
	suite
	has "hfsutils (toolchain/bin)" f "$TC/bin/hfsck"
	has "A/UX 3.1 CD (--aux-cd)" f "$AUX/media/aux-3.1.iso"
	has "A/UX 3.1 disk image, for images/mkimage.sh only (--aux-disk)" sh -c '[ -e "$1/AUX_3_1_1GB_Use_In_Shoebill.zip" ] || [ -e "$1/AUX_3_1_1GB.dsk" ]' - "$AUX"
	has "Quadra 800 ROM (--q800-rom)" f "$AUX/Quadra 800.ROM"
	has "tape segments 03 07 10" sh -c '[ -f "$1/03" ] && [ -f "$1/07" ] && [ -f "$1/10" ]' - "$T"
}
x11() {
	disk
	has "tape segments 13 14 (Xcore, Xbasic)" sh -c '[ -f "$1/13" ] && [ -f "$1/14" ]' - "$T"
	has "X11R6.3 sources (setup.sh --x11)" f "$AUX/ref/x11r6.3/dist/xc-3.tar.gz"
	has "x11r6.3-amix (setup.sh --x11)" f "$AUX/ref/x11r6.3-amix/overlay"
}
mac() {
	x11
	has "Mac OS 7.6.1 CD (--macos761)" f "$AUX/media/Mac OS 7.6.1.iso"
	has "Quadra 700 ROM (--q700-rom)" f "$AUX/420DBFF3 - Quadra 700&900 & PB140&170.ROM"
	has "A/UX root tree (tests/aux/auxroot)" sh -c 'd=$(cat "$1" 2>/dev/null) && [ -d "$d/mac" ]' - "$AUX/tests/aux/auxroot"
}
tos() {
	suite
	has "EmuTOS 1.4 (setup.sh --tos-src)" f "$AUX/ref/emutos-release/emutos-512k-1.4.zip"
	has "fVDI (setup.sh --tos-src)" f "$AUX/ref/fvdi"
}
amiga() {
	suite
	has "AmigaOS 3.2 CD (--amiga-cd)" f "$AUX/AmigaOS3.2CD.iso"
	has "Kickstart from the CD" f "$AUX/images/work/amiga-stage/media/ROM/kicka4000.rom"
}
falcon() {
	kernel
	has "tape segments 02 03 10" sh -c '[ -f "$1/02" ] && [ -f "$1/03" ] && [ -f "$1/10" ]' - "$T"
	has "EmuTOS 1.4 (setup.sh --tos-src)" f "$AUX/ref/emutos-release/emutos-512k-1.4.zip"
	has "cmake (for kernel/atari/build-hatari.sh)" sh -c 'command -v cmake || [ -d "$1/ref/hatari" ]' - "$AUX"
}

[ $# -gt 0 ] || set -- kernel suite disk x11 mac tos amiga falcon
want=$*
[ $# -gt 0 ] && [ "$want" = "kernel suite disk x11 mac tos amiga falcon" ] && all=1
bad=0
for t in "$@"; do
	case $t in kernel|suite|disk|x11|mac|tos|amiga|falcon) ;; *) echo "unknown target $t"; exit 2 ;; esac
	miss=0
	echo "$t:"
	out=$($t)
	if [ -n "$out" ]; then
		echo "$out" | sort -u
		echo "  -> incomplete"
		[ -n "$all" ] && [ "$t" != kernel ] || bad=1
	else
		echo "  -> ready"
	fi
done
exit $bad
