#!/bin/sh
# mktestroot.sh -- RAM-disk root plus /tests; init runs /tests/runall on the console.
#
#   [KDIR=kernel-tree] [KERNEL=elf] [IMG=out] [TESTKB=4096] [NET=1] [AMIGANET=1] [NOAUX=1] sh tests/mktestroot.sh
#
# KERNEL supplies the addresses in /tests/ksyms.  Default IMG: build/testroot.img.
# NOAUX=1 leaves out t_aux's A/UX files and modules (t_aux skips).
# TESTKB grows by the size of the A/UX files (the Mac environment's are
# several MB).  INITTAB replaces etc/inittab (e.g. to run fewer tests).
# NET=1: the network root instead (build/netroot.img, 6 MB): net/net.manifest,
# build/netbin, and init runs only t_net.  Under GROUP/ONLY without t_net,
# init brings the network up itself (net/inittab.up).
# AMIGANET=1: the test root with TCP/IP (the net files, loopback up) as well
# as the Amiga SYS: disk, for t_amiga's bsdsocket.library checks (build/
# amiganetroot.img; run-qemu.sh does this under AMIGANET=1).
set -e

[ -z "$AMIGANET" ] || NET=1
IMG0=$IMG
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
	if [ -n "$AMIGANET" ]; then
		M=$B/amiganetroot.manifest
		IMG=${IMG0:-$B/amiganetroot.img}
		INITTAB=$T/net/inittab.amiga
	elif [ -n "$ONLY" ]; then
		case " $ONLY " in *"/t_net.c "*) ;; *) INITTAB=$T/net/inittab.up ;; esac
	fi
	[ -f "$B/net07/usr/sbin/ping" ] || { echo "[FAIL] no build/net07 (run net/getnet.sh)"; exit 1; }
else
	M=$B/testroot.manifest
	IMG=${IMG:-$B/testroot.img}
	INITTAB=${INITTAB:-$T/etc/inittab}
fi

[ -f "$RD/build/core/sbin/init" ] || { echo "[FAIL] no $RD/build/core (run mkroot.sh)"; exit 1; }
[ -x "$B/bin/runall" ] || { echo "[FAIL] no tests/build/bin (run build.sh)"; exit 1; }

nm "$KERNEL" | awk '$3 ~ /^(freemem|availrmem|availsmem|lbolt|fpu_present|anoninfo|ticks_til_clock|mac_ticks|dlm_inited|sn_nintr|sn_nslot|guest_loading|M68Kvec|guest_chain|guest_nftrap|guest_nfpriv|idle|rd_unit|adb_nintr|adb_nsrq|segmapcnt|waitloc|kmeminfo|pageio_out|cputype|sysinfo|maxpgio|end)$/ && ($3 != "idle" || $2 == "T") { print $3, $1 }' \
	> "$B/ksyms"
# t_dlm's modules, built against this kernel (none without module support)
KDIR=$KDIR sh "$T/dlm/build.sh" "$KERNEL" "$B/dlm"
DSPB=$B/display
sh "$KDIR/mac/display/mods.sh" "$KERNEL" "$DSPB"
# t_kstat's module (none without module support)
SYSB=$B/sys
rm -rf "$SYSB"
! "$AUX/toolchain/bin/m68k-elf-nm" "$KERNEL" 2>/dev/null | grep -q ' T dlm_init$' ||
	sh "$KDIR/mods.sh" TARGET=$(sh "$KDIR/mods.sh" -t "$KERNEL") "$SYSB" KERNEL="$KERNEL" sys
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
# t_mint's mintrun and MiNTLib programs
MINTB=$B/mint
KDIR=$KDIR sh "$T/mint/build.sh" "$MINTB"
# t_cpm's startcpm and CP/M-68K
CPMB=$B/cpm
KDIR=$KDIR sh "$T/cpm/build.sh" "$CPMB"
# t_smsqe's startsmsq and SMSQ/E
SMQB=$B/smsqe
[ ! -f "$T/smsqe/build.sh" ] || KDIR=$KDIR sh "$T/smsqe/build.sh" "$SMQB"
# t_amiga's guest image, startamiga and ROM
AMIB=$B/amiga
KDIR=$KDIR sh "$T/amiga/build.sh" "$AMIB"
# t_win16's startwin and volume (run-qemu.sh adds it as SCSI disk 3)
W16B=$B/w16
case " $ONLY " in "  "|*/t_win16.c*|*/t_stable.c*) sh "$T/win16/mkvol.sh" "$W16B" ;; *) rm -rf "$W16B" ;; esac
# t_quake's volume (SCSI disk 5), only when asked for: a timedemo takes long
QKB=$B/quake
case " $ONLY " in */t_quake.c*) sh "$T/quake/mkvol.sh" "$QKB" ;; *) rm -rf "$QKB" ;; esac
# t_v200's volume (SCSI disk 4): its native host and the user's Flash dump
V2B=$B/v200
case " $ONLY " in "  "|*/t_v200.c*) sh "$T/v200/mkvol.sh" "$V2B" ;; *) rm -rf "$V2B" ;; esac
# t_next's volume (SCSI disk 6): its native host and the user's boot ROM
NXB=$B/next
case " $ONLY " in "  "|*/t_next.c*) sh "$T/next/mkvol.sh" "$NXB" ;; *) rm -rf "$NXB" ;; esac
# t_lisa's volume (SCSI disk 6): the user's Lisa volume, only when asked for
LSB=$B/lisa
case " $ONLY " in */t_lisa.c*) sh "$T/lisa/mkvol.sh" "$LSB" ;; *) rm -rf "$LSB" ;; esac
# t_ufs's volume: /d/f0../d/f3 in t_pattern (run-qemu.sh adds it as SCSI disk 2)
UFSB=$B/ufs
if [ -z "$NET" ]; then
	rm -rf "$UFSB"
	mkdir -p "$UFSB/src"
	echo "d /d 755 0 3" > "$UFSB/manifest"
	for k in 0 1 2 3; do
		python3 -c "import sys; sys.stdout.buffer.write(bytes((o * 131 + (o >> 9) * 7 + $k) & 255
			for o in range(3000 + 9000 * $k)))" > "$UFSB/src/f$k"
		echo "f /d/f$k 644 0 3 f$k" >> "$UFSB/manifest"
	done
	python3 "$KDIR/mac/diskroot/mkufs.py" -s 4 -t 723000000 -r "$UFSB/src" -m /ufs \
		"$UFSB/manifest" "$UFSB/ufs.img"
fi
if [ -z "$TESTKB" ]; then
	TESTKB=$((BASEKB + 128))
	[ -d "$AUXB/root" ] && TESTKB=$((TESTKB + 256 + $(du -sk "$AUXB/root" | cut -f1)))
	[ -d "$TOSB/root" ] && TESTKB=$((TESTKB + 1152 + $(du -sk "$TOSB/root" | cut -f1)))
	# t_env: XaAES in its two environments' copies, in /tmp
	[ -d "$TOSB/root/tos/xaaes" ] && TESTKB=$((TESTKB + 2 * $(du -sk "$TOSB/root/tos/xaaes" | cut -f1)))
	[ -d "$AMIB/root" ] && TESTKB=$((TESTKB + 64 + $(du -sk "$AMIB/root" | cut -f1)))
	[ -d "$SMQB/root" ] && TESTKB=$((TESTKB + 64 + 2 * $(du -sk "$SMQB/root" | cut -f1)))
	# t_cpm: S: and its scratch drives
	[ -d "$CPMB/root" ] && TESTKB=$((TESTKB + 128 + 3 * $(du -sk "$CPMB/root" | cut -f1)))
	[ ! -f "$B/bin/t_gl" ] || TESTKB=$((TESTKB + $(du -sk "$B/bin/t_gl" | cut -f1)))
	[ ! -f "$W16B/startwin" ] || TESTKB=$((TESTKB + 64 + $(du -sk "$W16B/startwin" | cut -f1)))
	# t_v200: the 4 MB Flash file, its running copy and the 256 KB RAM file in /tmp
	[ ! -f "$V2B/v200.img" ] || TESTKB=$((TESTKB + 8192 + 256 + 64))
	# t_next: the VRAM file and NVRAM in /tmp
	[ ! -f "$NXB/next.img" ] || TESTKB=$((TESTKB + 256 + 64))
	# t_lisa: the blocks the Lisa OS writes, in /tmp
	[ ! -f "$LSB/lisa.img" ] || TESTKB=$((TESTKB + 1024))
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
	grep -q '^c /dev/ptmx[[:space:]]' "$M" || echo "c /dev/ptmx 666 0 3 27 15"
	if [ -f "$UFSB/ufs.img" ]; then
		echo "b /dev/dsk/c2d0s0 600 0 3 18 2"
		echo "d /ufs 755 0 3"
	fi
	echo "c /dev/ticlts 666 0 3 27 30"
	grep -q '^d /dev/pts' "$M" || echo "d /dev/pts 755 0 3"
	for i in 0 1 2 3 4 5 6 7; do
		grep -q "^c /dev/pts/$i[[:space:]]" "$M" || echo "c /dev/pts/$i 620 0 7 14 $i"
	done
	echo "d /tests 755 0 3"
	echo "f /tests/ksyms 444 0 3 $B/ksyms"
	# t_arith's FP through awk
	grep -q '^f /usr/bin/awk ' "$M" || echo "f /usr/bin/awk 755 0 3 $RD/build/core/usr/bin/nawk"
	# t_scsi runs scsibench
	for f in dd sum time id expr; do
		grep -q "^f /usr/bin/$f[[:space:]]" "$M" || echo "f /usr/bin/$f 755 0 3 $RD/build/core/usr/bin/$f"
	done
	[ -z "$ONLY" ] || { : > "$B/.group"; echo "f /tests/.group 444 0 3 $B/.group"; }
	# STABLE: t_stable's environments and idle=SECONDS
	[ -z "$STABLE" ] || { echo "$STABLE" > "$B/stable.envs"; echo "f /tests/stable.envs 444 0 3 $B/stable.envs"; }
	for f in "$B"/bin/*; do
		# GROUP/ONLY: only the chosen tests (helpers that are not t_* stay)
		# t_stable runs hours, t_smsqe is out of the default suite: only when chosen
		case " $ONLY " in "  ") case $(basename "$f") in t_stable|t_smsqe|t_swp) continue ;; esac ;; *t_*) case $(basename "$f") in t_*)
			case " $ONLY " in *"/$(basename "$f").c "*) ;; *) continue ;; esac ;; esac ;; esac
		echo "f /tests/$(basename "$f") 755 0 3 $f"
	done
	if [ -n "$NET" ]; then
		[ -n "$AMIGANET" ] || for f in "$B"/netbin/*; do
			case " $ONLY " in "  ") ;; *"/$(basename "$f").c "*) ;; *) continue ;; esac
			echo "f /tests/$(basename "$f") 755 0 3 $f"
		done
		sed -e "s#@CORE@#$RD/build/core#" -e "s#@NET07@#$B/net07#" -e "s#@NETDIR@#$T/net#" "$T/net/net.manifest"
	fi
	[ -f "$AUXB/mod.d/uinter" ] && echo "c /dev/uinter0 666 0 3 54 0"
	# t_nativecall's and t_v200's devices
	[ -f "$AUXB/mod.d/nhcall" ] && echo "c /dev/nhcall 600 0 3 66 0"
	[ -f "$AUXB/mod.d/tiplatform" ] && echo "c /dev/tiplatform 660 0 25 65 0"
	[ -f "$AUXB/mod.d/nextguest" ] && echo "c /dev/nextguest 660 0 25 67 0"
	[ -f "$AUXB/mod.d/lisaguest" ] && echo "c /dev/lisa 660 0 3 68 0"
	if [ -d "$AUXB/mod.d" ]; then
		echo "d /tests/aux 755 0 3"
		echo "d /tests/aux/mod.d 755 0 3"
		for f in "$AUXB"/mod.d/*; do
			[ -z "$CPM_NO_TOS" ] || [ "$(basename "$f")" != tosguest ] || continue
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
		echo "f /usr/bin/macscrn 755 0 3 $B/bin/macscrn"
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
		# the Mac System Folders' ufs volume (run-qemu.sh adds it as SCSI disk 1)
		if [ -f "$AUXB/macsys.img" ]; then
			echo "b /dev/dsk/c1d0s0 600 0 3 18 1"
			echo "d /macsys 755 0 3"
		fi
	fi
	if [ -d "$MINTB/root" ] && [ -f "$AUXB/mod.d/tosguest" ]; then
		[ -d "$TOSB/root" ] || echo "c /dev/tos 660 0 25 56 0"
		(cd "$MINTB/root" && find . -type d | sed 's#^\.##' | sort) | while read -r d; do
			if [ -n "$d" ] && ! grep -q "^d $d[ 	]" "$M"; then
				echo "d $d 755 0 3"
			fi
		done
		(cd "$MINTB/root" && find . -type f | sed 's#^\.##' | sort) | while read -r f; do
			echo "f $f 755 0 3 $MINTB/root$f"
		done
	fi
	if [ -d "$CPMB/root" ] && [ -f "$AUXB/mod.d/cpmguest" ]; then
		echo "c /dev/cpm 660 0 25 59 0"
		(cd "$CPMB/root" && find . -type d | sed 's#^\.##' | sort) | while read -r d; do
			if [ -n "$d" ] && ! grep -q "^d $d[ 	]" "$M"; then
				echo "d $d 755 0 3"
			fi
		done
		(cd "$CPMB/root" && find . -type f | sed 's#^\.##' | sort) | while read -r f; do
			echo "f $f 755 0 3 $CPMB/root$f"
		done
		[ -z "$CPM_NO_TOS" ] || echo "f /tests/cpm/no-tos 444 0 3 $T/cpm/no-tos"
	fi
	if [ -d "$SMQB/root" ] && [ -d "$TOSB/root" ] && [ -f "$AUXB/mod.d/tosguest" ]; then
		(cd "$SMQB/root" && find . -type d | sed 's#^\.##' | sort) | while read -r d; do
			if [ -n "$d" ] && ! grep -q "^d $d[ 	]" "$M"; then
				echo "d $d 755 0 3"
			fi
		done
		(cd "$SMQB/root" && find . -type f | sed 's#^\.##' | sort) | while read -r f; do
			echo "f $f 755 0 3 $SMQB/root$f"
		done
	fi
	if [ -z "$CPM_NO_TOS" ] && [ -d "$TOSB/root" ] && [ -f "$AUXB/mod.d/tosguest" ]; then
		echo "c /dev/tos 660 0 25 56 0"
		# starttos copies environments with these
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
	if [ -f "$W16B/win16.img" ]; then
		echo "f /usr/bin/startwin 755 0 3 $W16B/startwin"
		for l in libsocket.so libnsl.so; do
			grep -q "^f /usr/lib/$l[[:space:]]" "$M" || echo "f /usr/lib/$l 755 2 2 $RD/build/core/usr/lib/$l"
		done
		echo "b /dev/dsk/c3d0s0 600 0 3 18 3"
		echo "d /win 755 0 3"
	fi
	if [ -f "$QKB/quake.img" ]; then
		echo "b /dev/dsk/c5d0s0 600 0 3 18 5"
		echo "d /quake 755 0 3"
	fi
	if [ -f "$V2B/v200.img" ]; then
		echo "b /dev/dsk/c4d0s0 600 0 3 18 4"
		echo "d /v200 755 0 3"
	fi
	if [ -f "$LSB/lisa.img" ]; then
		echo "b /dev/dsk/c6d0s0 600 0 3 18 6"
		echo "d /lisa 755 0 3"
	fi
	if [ -f "$NXB/next.img" ]; then
		echo "b /dev/dsk/c6d0s0 600 0 3 18 6"
		echo "d /next 755 0 3"
	fi
	if [ -f "$AMIB/sys.img" ] || [ -f "$UFSB/ufs.img" ] || [ -f "$V2B/v200.img" ] || [ -f "$NXB/next.img" ] || [ -f "$LSB/lisa.img" ] || [ -f "$AUXB/macsys.img" ] ||
	    [ -f "$W16B/win16.img" ] || [ -f "$QKB/quake.img" ]; then
		echo "d /usr/lib/fs/ufs 755 0 3"
		echo "f /usr/lib/fs/ufs/mount 555 0 3 $RD/build/core/usr/lib/fs/ufs/mount"
	fi
	if [ -f "$B/otb/mod.d/otbridge" ]; then
		echo "d /tests/otb 755 0 3"
		echo "f /tests/otb/otbridge 644 0 3 $B/otb/mod.d/otbridge"
		grep -q '^c /dev/otbridge' "$M" || echo "c /dev/otbridge 666 0 3 55 0"
	fi
	# t_sound's drivers, built with the kernel; the default module path
	if [ -f "$DSPB/mod.d/dsuser" ] || [ -f "$KDIR/build/mac/sound/mod.d/asc" ] ||
	    [ -f "$KDIR/build/mac/scsi/mod.d/ncr96" ] || [ -f "$SYSB/mod.d/kstat" ]; then
		echo "d /etc/conf 755 0 3"
		echo "d /etc/conf/mod.d 755 0 3"
	fi
	if [ -f "$DSPB/mod.d/dsuser" ]; then
		echo "f /etc/conf/mod.d/dsuser 644 0 3 $DSPB/mod.d/dsuser"
	fi
	if [ -f "$KDIR/build/mac/sound/mod.d/asc" ]; then
		for f in asc auxsnd; do
			echo "f /etc/conf/mod.d/$f 644 0 3 $KDIR/build/mac/sound/mod.d/$f"
		done
	fi
	if [ -f "$SYSB/mod.d/kstat" ]; then
		echo "f /etc/conf/mod.d/kstat 644 0 3 $SYSB/mod.d/kstat"
		echo "c /dev/kstat 444 0 3 69 0"
	fi
	# the SCSI controller, loaded on the first disk open
	if [ -f "$KDIR/build/mac/scsi/mod.d/ncr96" ]; then
		echo "f /etc/conf/mod.d/ncr96 644 0 3 $KDIR/build/mac/scsi/mod.d/ncr96"
	fi
	if [ -d "$B/dlm/mod.d" ]; then
		echo "d /tests/mod.d 755 0 3"
		for f in "$B"/dlm/mod.d/*; do
			echo "f /tests/mod.d/$(basename "$f") 644 0 3 $f"
		done
	fi
} >> "$M"

python3 "$RD/mks5fs.py" -s "$TESTKB" -i 1024 -b 1024 -t 723000000 "$M" "$IMG"
L=${IMG%.img}.lst
python3 "$RD/s5check.py" "$IMG" -l > "$L" || { cat "$L"; exit 1; }
tail -2 "$L"
