#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
if [ "$#" -gt 1 ]; then
	echo 'usage: check.sh [kicka4000.rom]' >&2
	exit 2
fi
sh "$D/run.sh"
sh "$D/fault.sh"
sh "$D/blit.sh"
sh "$D/video.sh"
sh "$D/rtgtransport.sh"
sh "$D/hostfs.sh"
sh "$D/broker.sh"
sh "$D/input.sh"
sh "$D/sysroot.sh"
nice -n 19 python3 "$AUX/tools/amiga/test_media.py"
nice -n 19 python3 "$AUX/tools/amiga/test_adf.py"
for script in "$AUX/kernel/guest/amiga/build.sh" \
    "$AUX/kernel/guest/amiga/dos/build.sh" \
    "$AUX/kernel/guest/amiga/input/build.sh" \
    "$AUX/kernel/guest/mod/amigaguest/compile.sh" \
    "$AUX/kernel/guest/mod/amigaguest/build.sh" \
    "$AUX/images/amigaenv/mkamiga.sh" "$AUX/images/amigaenv/makeamiga" \
    "$AUX/images/amigaenv/installmig"; do
	sh -n "$script"
done
if [ "$#" -eq 1 ]; then
	nice -n 19 python3 "$AUX/tools/amiga/media.py" check-rom "$1"
fi
echo 'Amiga host checks passed; target execution remains unverified.'
