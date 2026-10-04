#!/bin/sh
# build.sh -- AMIX 2.1c base, the 68040 port, then the Mac kernel.
#
#   sh kernel/build.sh
#
# AMIX_TAPE=<dir with tape segment 02> rebuilds the RAM-disk root image;
# without it an existing mac/ramdisk/build/root.img is checked and used.
#
# PLATFORM=atari builds the Falcon030 kernel (build/unix-atari030.elf)
# from the stage-1 base instead of the Mac kernel.
#
# Logs go to kernel/build/logs/.  One PASS/FAIL line per stage; stops at
# the first failure with a non-zero exit.
set -e

K=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$K/.." && pwd)
PORT="$K/amix-040-060-port"
LOG="$K/build/logs"
BASE_SHA=b37cb0edfdb0b078e70d28466c9749a87db7170a4028b9b85a38c219527f1af0
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
mkdir -p "$LOG"

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1: $2 (log: $LOG/$1.log)"; exit 1; }

# 1. layout-matched 2.1c base kernel
{
	sh "$K/tools/mk-kit21c.sh" &&
	KIT="$K/kit-2.1c/usr/sys" sh "$K/tools/relink-kit.sh" "$K/amix-2.1p2a"
} > "$LOG/kit-2.1c.log" 2>&1 || fail kit-2.1c "kit relink failed"
sha=$(sha256sum "$K/amix-2.1p2a/stand/unix" | cut -d' ' -f1)
[ "$sha" = "$BASE_SHA" ] || fail kit-2.1c "sha256 $sha"
pass kit-2.1c

if [ "${PLATFORM:-mac}" = atari ]; then
	sh "$K/atari/relink-atari.sh" > "$LOG/atari.log" 2>&1 || fail atari "relink-atari.sh failed"
	pass atari
	exit 0
fi

# 2. the port's 68040 kernel
sh "$PORT/relink-040.sh" > "$LOG/port.log" 2>&1 || fail port "relink-040.sh failed"
grep -q 'TOTAL complaints: 0 ' "$LOG/port.log" || fail port "validator complaints"
pass port

# 3. RAM-disk root image (linked in by stage 4 when present)
IMG="$K/mac/ramdisk/build/root.img"
: > "$LOG/root.log"
if [ -f "${AMIX_TAPE:-/nonexistent}/02" ]; then
	sh "$K/mac/display/build.sh" > "$LOG/root.log" 2>&1 || fail root "display build (dstest) failed"
	sh "$K/mac/ramdisk/mkroot.sh" >> "$LOG/root.log" 2>&1 || fail root "mkroot.sh failed"
fi
if [ -f "$IMG" ]; then
	python3 "$K/mac/ramdisk/s5check.py" "$IMG" >> "$LOG/root.log" 2>&1 ||
		fail root "s5check.py"
	pass root
else
	pass "root (no image: root from the boot record or root=)"
fi

# 4. Mac overlay on build/unix-040
sh "$K/mac/relink-mac.sh" > "$LOG/mac.log" 2>&1 || fail mac "relink-mac.sh failed"
pass mac

# 5. static checks on the Mac image
ELF="$K/build/unix-mac.elf"
{
	u=$(m68k-elf-nm -u "$ELF" | wc -l)
	echo "unresolved: $u"
	[ "$u" -eq 0 ] &&

	python3 "$PORT/src/check_relink_relocs.py" "$K/build/unix-mac" > "$LOG/validator.log" &&
	grep 'TOTAL complaints: 0 ' "$LOG/validator.log" &&

	m68k-elf-readelf -h "$ELF" | grep -q 'Type: *EXEC' &&
	[ "$(m68k-elf-readelf -l "$ELF" | grep -c '^ *LOAD')" -eq 2 ] &&
	entry=$(m68k-elf-readelf -h "$ELF" | awk '/Entry point/{print $4}') &&
	me=$(m68k-elf-nm "$ELF" | awk '$3=="mac_entry"{print $1}') &&
	echo "entry $entry mac_entry 0x$me" &&
	[ $((entry)) -eq $((0x$me)) ] &&

	"$K/build/mac/dlm/mkksym" -c "$ELF" &&
	python3 "$K/dlm/tools/chkdlm.py" "$ELF" &&
	python3 "$K/guest/chkguest.py" "$ELF" "$K/build/mac/guest/gates.lst" &&

	st=$(m68k-elf-nm "$ELF" | awk '$3=="stext"{print $1}') &&
	m68k-elf-objdump -d --start-address=0x$st --stop-address=$((0x$st + 0x60)) "$ELF" \
		> "$LOG/stext.dis" &&
	! grep -q 'f000 2400' "$LOG/stext.dis" &&
	echo "stext: no 030 pflusha"
} > "$LOG/checks.log" 2>&1 || fail checks "see log"
pass checks

# 6. A/UX Startup COFF, checked against launch's rules
COFF="$K/build/unix-mac.coff"
{
	sh "$K/mac/boot/mkcoff.sh" "$ELF" "$COFF" &&
	"$K/mac/boot/build/elf2coff" -c "$COFF"
} > "$LOG/coff.log" 2>&1 || fail coff "see log"
tail -1 "$LOG/coff.log" | grep -q ACCEPTED || fail coff "launch rules"
pass coff
