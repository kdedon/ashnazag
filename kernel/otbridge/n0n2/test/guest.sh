#!/bin/sh
# guest.sh -- run t_otbst and t_aslm on the Quadra 800 model (QEMU q800,
# user-mode network) with a kernel that has otbridge.diff applied.
#
#   sh kernel/otbridge/n0n2/test/guest.sh TREE
#
# TREE is a scratch copy of the repository with kernel/ patched and built
# (sh kernel/build.sh), tests/ copied (results, s5disk and build left out;
# build/net07 kept) and toolchain linked in.  Only TREE is changed: its
# tests get t_otbst (otb/), t_aslm (src/), the driver image, mkaslm for
# AMIX, a library it built, malformed copies of it and the CD's
# AppleTalk library (/tests/n0n2),
# and a network inittab running t_net, t_otbst and t_aslm.
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$N/../../.." && pwd)
TREE=$(cd "$1" && pwd)
T=$TREE/tests
B=$T/build/n0n2
TB=$AUX/toolchain/bin
TA=$AUX/toolchain/amix/bin
NICE="nice -n 19"
mkdir -p "$B"

"$TB/m68k-elf-as" -m68020 -o "$B/enet.o" "$N/enet/enet.s"
"$TB/m68k-elf-objcopy" -O binary -j .text "$B/enet.o" "$B/enet.drvr"
rm "$B/enet.o"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
W=$T/build/n0n2work
mkdir -p "$W"
$NICE ${HOSTCC:-cc} -std=gnu89 -O -o "$W/mkaslm" "$N/mkaslm/mkaslm.c"
$NICE "$TA/m68k-cbm-sysv4-gcc" -O -m68020 -fcall-used-d2 -o "$B/mkaslm" "$N/mkaslm/mkaslm.c"
"$TB/m68k-elf-as" -m68020 -o "$W/aslmrt.o" "$N/mkaslm/aslmrt.s"
$NICE "$TA/m68k-cbm-sysv4-gcc" -O -m68020 -fcall-used-d2 -c -o "$W/auxtest.o" \
	"$N/mkaslm/test/auxtest.c"
"$TB/m68k-elf-ld" -r -d -o "$W/lib.o" "$W/aslmrt.o" "$W/auxtest.o"
"$W/mkaslm" -n 'AUXLib$test' -e 'AUXTest$fset,0x110,0x100,AUXTestAdd,AUXTestMsg,AUXTestCount' \
	-u 'AUXTest$idx,0x100,0x100,AUXTestCount' -o "$B/auxtest.bin" "$W/lib.o"
python3 "$N/test/badfiles.py" "$B/auxtest.bin" "$W/bad" 40
rm -f "$B"/bad*
i=0
for f in "$W"/bad/*.bin; do		# s5 names: 14 characters
	i=$((i + 1))
	cp "$f" "$B/bad$i"
done
ISO=$AUX/media/MacOS8_1.iso
if [ -f "$ISO" ]; then
	(
		export HOME="$W"
		"$TB/hmount" "$ISO" > /dev/null
		"$TB/hcd" "System Folder"
		"$TB/hcd" Extensions
		"$TB/hcopy" -m "Open Tpt AppleTalk Library" "$B/atalk.bin"
		"$TB/humount" > /dev/null
	)
fi

cp "$N/test/t_otbst.c" "$T/otb/"
cp "$N/test/t_aslm.c" "$T/src/"
grep -q 'n0n2' "$T/mktestroot.sh" || python3 - "$T/mktestroot.sh" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
at = '\tif [ -d "$B/dlm/mod.d" ]; then'
add = '''\tif [ -d "$B/n0n2" ]; then
\t\techo "d /tests/n0n2 755 0 3"
\t\tfor f in "$B"/n0n2/*; do
\t\t\techo "f /tests/n0n2/$(basename "$f") 755 0 3 $f"
\t\tdone
\tfi
'''
assert s.count(at) == 1
open(p, 'w').write(s.replace(at, add + at))
PY
sed -i 's|^tt::sysinit:.*|tt::sysinit:/tests/runall t_net:300 t_otbst:200 t_aslm:240 </dev/console >/dev/console 2>\&1|' \
	"$T/net/inittab"

while [ -n "$(ps -C qemu-system-m68k -o pid=)" ]; do sleep 1; done
MEM=${MEM:-128} TESTKB=${TESTKB:-9216} TMO=${TMO:-900} sh "$T/run-qemu.sh" --net --kernel-dir "$TREE/kernel"
