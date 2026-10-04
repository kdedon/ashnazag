#!/bin/sh
# host.sh -- host tests of mkaslm and the .ENET driver image.
#
#   sh kernel/otbridge/n0n2/test/host.sh [workdir]
#
# Round trip and layout checks of the three Open Transport libraries and
# the Shared Library Manager from the Mac OS 8.1 CD (media/MacOS8_1.iso,
# skipped without it), a library built from test/auxtest.c, builds that
# must fail, and malformed files (test/badfiles.py).  One PASS/FAIL line
# per check; exit 1 on any FAIL.
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$N/../../.." && pwd)
W=${1:-${TMPDIR:-/tmp}/n0n2-host}
TB=$AUX/toolchain/bin
TA=$AUX/toolchain/amix/bin
NICE="nice -n 19"
mkdir -p "$W"
rc=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1${2:+: $2}"; rc=1; }
expect() {	# name want-exit command...
	n=$1; want=$2; shift 2
	if "$@" > "$W/$n.out" 2>&1; then got=0; else got=$?; fi
	if [ "$got" = "$want" ]; then pass "$n"; else fail "$n" "exit $got"; tail -3 "$W/$n.out"; fi
}

$NICE ${HOSTCC:-cc} -std=gnu89 -O -Wall -o "$W/mkaslm" "$N/mkaslm/mkaslm.c"
M=$W/mkaslm

# the CD's libraries
ISO=$AUX/media/MacOS8_1.iso
if [ -f "$ISO" ] && command -v "$TB/hmount" > /dev/null; then
	(
		export HOME="$W"	# hfsutils keeps its state in ~/.hcwd
		"$TB/hmount" "$ISO" > /dev/null
		"$TB/hcd" "System Folder"
		"$TB/hcd" Extensions
		for f in "Open Transport Library" "Open Tpt Internet Library" \
		    "Open Tpt AppleTalk Library" "Shared Library Manager"; do
			"$TB/hcopy" -m "$f" "$W/$(echo "$f" | tr ' ' _).bin"
		done
		"$TB/humount" > /dev/null
	)
	for f in "$W"/Open_*.bin "$W"/Shared_Library_Manager.bin; do
		b=$(basename "$f" .bin)
		expect "roundtrip.$b" 0 "$M" -t "$f"
		expect "layout.$b" 0 "$M" -l "$f"
	done
	grep -q 'OTModl\$ddp' "$W/layout.Open_Tpt_AppleTalk_Library.out" || true
	"$M" -d "$W/Open_Transport_Library.bin" > "$W/describe.out"
	grep -q 'fset  OTModl\$enetDRVR 0110/0110' "$W/describe.out" &&
		pass describe.enetDRVR || fail describe.enetDRVR
else
	echo "SKIP cd: no $ISO or hfsutils"
fi

# our library
"$TB/m68k-elf-as" -m68020 -o "$W/aslmrt.o" "$N/mkaslm/aslmrt.s"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
$NICE "$TA/m68k-cbm-sysv4-gcc" -O -m68020 -fcall-used-d2 -c -o "$W/auxtest.o" \
	"$N/mkaslm/test/auxtest.c"
"$TB/m68k-elf-ld" -r -d -o "$W/lib.o" "$W/aslmrt.o" "$W/auxtest.o"
SPEC='AUXTest$fset,0x110,0x100,AUXTestAdd,AUXTestMsg,AUXTestCount'
expect build 0 "$M" -n 'AUXLib$test' -e "$SPEC" -u 'AUXTest$idx,0x100,0x100,AUXTestCount' \
	-o "$W/auxtest.bin" "$W/lib.o"
expect ours.roundtrip 0 "$M" -t "$W/auxtest.bin"
expect ours.layout 0 "$M" -l "$W/auxtest.bin"
grep -q 'export AUXTest\$fset: 3 functions by name' "$W/ours.layout.out" &&
	pass ours.exports || fail ours.exports
expect ours.appledouble 0 "$M" -n 'AUXLib$test' -e "$SPEC" -f ad -o "$W/auxtest.ad" "$W/lib.o"
expect ours.appledouble_reads 0 "$M" -l "$W/auxtest.ad"

# builds that must fail
expect fail.no_runtime 2 "$M" -e "$SPEC" -o "$W/x.bin" "$W/auxtest.o"
printf '\t.section .a5init,"ax"\n\t.globl __aslm_entry\n__aslm_entry:\n\tbsr.w\tf\n\t.globl __aslm_blk\n__aslm_blk:\n\t.text\n\t.globl __aslm_main0\n__aslm_main0:\n\trts\n\t.globl f\nf:\trts\n' > "$W/xseg.s"
"$TB/m68k-elf-as" -o "$W/xseg.o" "$W/xseg.s"
expect fail.pc_across_segments 2 "$M" -o "$W/x.bin" "$W/xseg.o"
printf 'extern long nowhere(); long g() { return nowhere(); }\n' > "$W/undef.c"
$NICE "$TA/m68k-cbm-sysv4-gcc" -O -c -o "$W/undef.o" "$W/undef.c"
"$TB/m68k-elf-ld" -r -d -o "$W/undef2.o" "$W/aslmrt.o" "$W/undef.o"
expect fail.undefined 2 "$M" -o "$W/x.bin" "$W/undef2.o"
expect fail.missing_export 2 "$M" -e 'X$y,1,1,nosuch' -o "$W/x.bin" "$W/lib.o"

# a damaged library: Main's first jump table entry beyond the segment
python3 - "$W/auxtest.bin" "$W/bad.bin" <<'EOF'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
i = d.index(b'\x00\x02\xa9\xf0')		# first Main entry
d[i + 4:i + 8] = b'\x00\x10\x00\x00'
open(sys.argv[2], 'wb').write(d)
EOF
expect layout.damaged 1 "$M" -l "$W/bad.bin"

# hostile input: broken containers, maps, fields and 200 random mutations,
# under the address and undefined-behaviour sanitizers when the compiler has
# them; every run must end with exit 0, 1 or 2 and no sanitizer report
python3 "$N/test/badfiles.py" "$W/auxtest.bin" "$W/bad" 200
MS=$W/mkaslm.san
if ${HOSTCC:-cc} -std=gnu89 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
    -o "$MS" "$N/mkaslm/mkaslm.c" 2> /dev/null; then
	export ASAN_OPTIONS=detect_leaks=0
else
	MS=$M
fi
nb=0
for f in "$W"/bad/*.bin; do
	case $(basename "$f") in fuzz*) modes=-l ;; *) modes="-d -t -l" ;; esac
	for m in $modes; do
		if "$MS" $m "$f" > "$W/bad.out" 2>&1; then got=0; else got=$?; fi
		if [ "$got" -gt 2 ] || grep -q 'Sanitizer\|runtime error' "$W/bad.out"; then
			fail "hostile.$(basename "$f" .bin)$m" "exit $got"
			nb=$((nb + 1))
		fi
	done
done
[ $nb -eq 0 ] && pass "hostile ($(ls "$W"/bad | wc -l) files)"
expect hostile.control 0 "$M" -l "$W/bad/rebuilt_good.bin"
for c in mb_truncated ad_offset_wraps type_count_huge res_length_huge libi_count_wraps \
    jt_length_huge seg_a5rel_huge reloc_distance_huge blk_nrel_wraps; do
	if "$M" -l "$W/bad/$c.bin" > /dev/null 2>&1; then fail "refused.$c"; else pass "refused.$c"; fi
done

# the driver image: position independent, header offsets, glue table
"$TB/m68k-elf-as" -m68020 -o "$W/enet.o" "$N/enet/enet.s"
if "$TB/m68k-elf-objdump" -r "$W/enet.o" | grep -q R_68K; then
	fail enet.pic "relocations in the image"
else
	pass enet.pic
fi
"$TB/m68k-elf-objcopy" -O binary -j .text "$W/enet.o" "$W/enet.drvr"
python3 - "$W/enet.drvr" <<'EOF' && pass enet.header || fail enet.header
import sys, struct
d = open(sys.argv[1], 'rb').read()
fl, = struct.unpack('>H', d[:2])
offs = struct.unpack('>5H', d[8:18])
ok = fl == 0x4400 and d[18:24] == b'\x05.ENET' and all(0 < o < len(d) and o % 2 == 0 for o in offs)
g = d.find(b'AUXG')
ok = ok and g > 0 and struct.unpack('>H', d[g + 4:g + 6])[0] == 5
ok = ok and d[g + 6:g + 8] == b'\xa7\x1e' and d[g + 38:g + 42] == b'\x2f\x38\x08\xfc'
sys.exit(0 if ok else 1)
EOF
exit $rc
