#!/bin/sh
# mktestroot.sh -- RAM-disk root plus /tests; init runs /tests/runall on the console.
#
#   [KDIR=kernel-tree] [KERNEL=elf] [IMG=out] [TESTKB=4096] [NET=1] [NOAUX=1] sh tests/mktestroot.sh
#
# KERNEL supplies the addresses in /tests/ksyms.  Default IMG: build/testroot.img.
# NOAUX=1 leaves out t_aux's A/UX files and modules (t_aux skips).
# TESTKB grows by the size of the A/UX files (the Mac environment's are
# several MB).  INITTAB replaces etc/inittab (e.g. to run fewer tests).
# NET=1: the network root instead (build/netroot.img, 6 MB): net/net.manifest,
# build/netbin, and init runs only t_net.
set -e

T=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
RD=$KDIR/mac/ramdisk
B=$T/build
KERNEL=${KERNEL:-$KDIR/build/unix-mac.elf}
BASEKB=4096
if [ -n "$NET" ]; then
	BASEKB=6144
	M=$B/netroot.manifest
	IMG=${IMG:-$B/netroot.img}
	INITTAB=$T/net/inittab
	[ -f "$B/net07/usr/sbin/ping" ] || { echo "[FAIL] no build/net07 (run net/getnet.sh)"; exit 1; }
else
	M=$B/testroot.manifest
	IMG=${IMG:-$B/testroot.img}
	INITTAB=${INITTAB:-$T/etc/inittab}
fi

[ -f "$RD/build/core/sbin/init" ] || { echo "[FAIL] no $RD/build/core (run mkroot.sh)"; exit 1; }
[ -x "$B/bin/runall" ] || { echo "[FAIL] no tests/build/bin (run build.sh)"; exit 1; }

nm "$KERNEL" | awk '$3 ~ /^(freemem|availrmem|availsmem|lbolt|fpu_present|anoninfo|ticks_til_clock|mac_ticks|dlm_inited|sn_nintr|sn_nslot|guest_loading|guest_nftrap|guest_nfpriv|rd_unit|adb_nintr|adb_nsrq)$/ { print $3, $1 }' \
	> "$B/ksyms"
# t_dlm's modules, built against this kernel (none without module support)
KDIR=$KDIR sh "$T/dlm/build.sh" "$KERNEL" "$B/dlm"
# t_otb's module (none without otbridge or its kernel linkages)
KDIR=$KDIR sh "$T/otb/build.sh" "$KERNEL" "$B/otb"
# t_aux's modules and A/UX programs (none without guest support)
AUXB=$B/aux
if [ -n "$NOAUX" ]; then
	AUXB=$B/noaux
	rm -rf "$AUXB"
else
	KDIR=$KDIR sh "$T/aux/build.sh" "$KERNEL" "$AUXB"
fi
# t_tos's launcher, cartridge, C: folder and ROM (none without the ROM)
TOSB=$B/tos
KDIR=$KDIR sh "$T/tos/build.sh" "$TOSB"
# t_amiga's guest image, startmig and ROM
AMIB=$B/amiga
KDIR=$KDIR sh "$T/amiga/build.sh" "$AMIB"
if [ -z "$TESTKB" ]; then
	TESTKB=$((BASEKB + 128))
	[ -d "$AUXB/root" ] && TESTKB=$((TESTKB + 256 + $(du -sk "$AUXB/root" | cut -f1)))
	[ -d "$TOSB/root" ] && TESTKB=$((TESTKB + 1152 + $(du -sk "$TOSB/root" | cut -f1)))
	[ -d "$AMIB/root" ] && TESTKB=$((TESTKB + 64 + $(du -sk "$AMIB/root" | cut -f1)))
fi

# /etc/group with the display devices' group
{ grep -v '^display:' "$RD/build/core/etc/group"; echo "display::25:"; } > "$B/group"
awk -v rd="$RD" '
	$1 == "f" && ($2 == "/etc/inittab" || $2 == "/etc/group") { next }
	$1 == "f" && substr($NF, 1, 1) != "/" { $NF = rd "/" $NF }
	{ print }' "$RD/root.manifest" > "$M"
grep -q '^h /etc/sulogin' "$M" || echo "h /etc/sulogin /sbin/sh" >> "$M"
{
	echo "f /etc/inittab 644 0 3 $INITTAB"
	echo "f /etc/group 444 0 3 $B/group"
	# SCC channel B: t_display's line to the host
	grep -q '^d /dev/term' "$M" || echo "d /dev/term 755 0 3"
	grep -q '^c /dev/term/b' "$M" || echo "c /dev/term/b 620 0 7 0 1"
	# clone is major 27; ptmx 15, pts 14, ticlts 30
	echo "c /dev/ptmx 666 0 3 27 15"
	echo "c /dev/ticlts 666 0 3 27 30"
	grep -q '^d /dev/pts' "$M" || echo "d /dev/pts 755 0 3"
	for i in 0 1 2 3 4 5 6 7; do echo "c /dev/pts/$i 620 0 7 14 $i"; done
	echo "d /tests 755 0 3"
	echo "f /tests/ksyms 444 0 3 $B/ksyms"
	# t_arith's FP through awk
	grep -q '^f /usr/bin/awk ' "$M" || echo "f /usr/bin/awk 755 0 3 $RD/build/core/usr/bin/nawk"
	for f in "$B"/bin/*; do
		echo "f /tests/$(basename "$f") 755 0 3 $f"
	done
	if [ -n "$NET" ]; then
		for f in "$B"/netbin/*; do
			echo "f /tests/$(basename "$f") 755 0 3 $f"
		done
		sed -e "s#@CORE@#$RD/build/core#" -e "s#@NET07@#$B/net07#" -e "s#@NETDIR@#$T/net#" "$T/net/net.manifest"
	fi
	[ -f "$AUXB/mod.d/uinter" ] && echo "c /dev/uinter0 666 0 3 54 0"
	if [ -d "$AUXB/mod.d" ]; then
		echo "d /tests/aux 755 0 3"
		echo "d /tests/aux/mod.d 755 0 3"
		for f in "$AUXB"/mod.d/*; do
			echo "f /tests/aux/mod.d/$(basename "$f") 644 0 3 $f"
		done
	fi
	if [ -n "$(ls "$AUXB/bin" 2>/dev/null)" ]; then
		echo "d /aux 755 0 3"
		echo "d /aux/bin 755 0 3"
		for f in "$AUXB"/bin/*; do
			echo "f /aux/bin/$(basename "$f") 755 0 3 $f"
		done
	fi
	# A/UX files at their A/UX paths (/shlib, terminfo, termcap)
	if [ -d "$AUXB/root" ]; then
		# the startmac script and what it runs
		echo "f /tests/startmac.sh 755 0 3 $AUX/images/macenv/startmac"
		echo "f /usr/bin/envlock 755 0 3 $B/bin/envlock"
		for c in cmp expr nawk; do
			grep -q "^f /usr/bin/$c " "$M" || echo "f /usr/bin/$c 755 0 3 $RD/build/core/usr/bin/$c"
		done
		grep -q '^f /usr/bin/grep ' "$M" || echo "f /usr/bin/grep 755 0 3 $RD/build/core/sbin/grep"
		(cd "$AUXB/root" && find . -type d | sed 's#^\.##' | sort) | while read -r d; do
			if [ -n "$d" ] && ! grep -q "^d $d[ 	]" "$M"; then
				echo "d $d 755 0 3"
			fi
		done
		(cd "$AUXB/root" && find . -type f | sed 's#^\.##' | sort) | while read -r f; do
			echo "f $f 755 0 3 $AUXB/root$f"
		done
	fi
	if [ -d "$TOSB/root" ] && [ -f "$AUXB/mod.d/tosguest" ]; then
		echo "c /dev/tos 660 0 25 56 0"
		# maketos's tools
		for c in cpio find expr; do
			grep -q "^f /usr/bin/$c " "$M" || echo "f /usr/bin/$c 755 0 3 $RD/build/core/usr/bin/$c"
		done
		grep -q '^f /usr/bin/grep ' "$M" || echo "f /usr/bin/grep 755 0 3 $RD/build/core/sbin/grep"
		(cd "$TOSB/root" && find . -type d | sed 's#^\.##' | sort) | while read -r d; do
			if [ -n "$d" ] && ! grep -q "^d $d[ 	]" "$M"; then
				echo "d $d 755 0 3"
			fi
		done
		(cd "$TOSB/root" && find . -type f | sed 's#^\.##' | sort) | while read -r f; do
			echo "f $f 755 0 3 $TOSB/root$f"
		done
	fi
	if [ -d "$AMIB/root" ] && [ -f "$AUXB/mod.d/amigaguest" ]; then
		echo "c /dev/amiga 660 0 25 57 0"
		# SYS: on a ufs volume (run-qemu.sh adds it as SCSI disk 0)
		if [ -f "$AMIB/sys.img" ]; then
			echo "b /dev/dsk/c0d0s0 600 0 3 18 0"
			echo "d /usr/lib/fs/ufs 755 0 3"
			echo "f /usr/lib/fs/ufs/mount 555 0 3 $RD/build/core/usr/lib/fs/ufs/mount"
			grep -q "^d /amiga[ 	]" "$M" || echo "d /amiga 755 0 3"
			echo "d /amiga/sys 755 0 3"
		fi
		(cd "$AMIB/root" && find . -type d | sed 's#^\.##' | sort) | while read -r d; do
			if [ -n "$d" ] && ! grep -q "^d $d[ 	]" "$M"; then
				echo "d $d 755 0 3"
			fi
		done
		(cd "$AMIB/root" && find . -type f | sed 's#^\.##' | sort) | while read -r f; do
			echo "f $f 755 0 3 $AMIB/root$f"
		done
	fi
	if [ -f "$B/otb/mod.d/otbridge" ]; then
		echo "d /tests/otb 755 0 3"
		echo "f /tests/otb/otbridge 644 0 3 $B/otb/mod.d/otbridge"
		grep -q '^c /dev/otbridge' "$M" || echo "c /dev/otbridge 666 0 3 55 0"
	fi
	if [ -d "$B/dlm/mod.d" ]; then
		echo "d /tests/mod.d 755 0 3"
		for f in "$B"/dlm/mod.d/*; do
			echo "f /tests/mod.d/$(basename "$f") 644 0 3 $f"
		done
	fi
} >> "$M"

python3 "$RD/mks5fs.py" -s "$TESTKB" -i 512 -b 1024 -t 723000000 "$M" "$IMG"
L=${IMG%.img}.lst
python3 "$RD/s5check.py" "$IMG" -l > "$L" || { cat "$L"; exit 1; }
tail -2 "$L"
