#!/bin/sh
# mkroot.sh -- build the RAM-disk root filesystem image.
#
#   sh ramdisk/mkroot.sh [tape-dir]
#
# tape-dir: AMIX 2.1 tape segments (02 = core, SVR4 cpio); default $AMIX_TAPE.
# Not needed once build/core has been extracted.
# Out: build/root.img (s5, 1 KB blocks), checked and listed in build/root.lst.
# Size and inode count: RDKB (default 1024), RDINODES (default 256).
set -e

RD=$(cd "$(dirname "$0")" && pwd)
TAPE="${1:-$AMIX_TAPE}"
B="$RD/build"
RDKB="${RDKB:-1024}"
RDINODES="${RDINODES:-256}"
STAMP=723000000		# 1992-11-29, fixed for reproducible images

mkdir -p "$B"
[ -f "$TAPE/02" ] || [ -f "$B/core/sbin/init" ] || {
	echo "[FAIL] no tape segment 02 in '$TAPE' and no build/core"; exit 1; }

if [ ! -f "$B/core/sbin/init" ]; then
	echo "[*] extracting tape segment 02 to build/core"
	rm -rf "$B/core"; mkdir -p "$B/core"
	# device nodes cannot be made without root; they are in the manifest
	(cd "$B/core" && cpio -idm --no-absolute-filenames --quiet < "$TAPE/02" 2>/dev/null) || true
fi

python3 "$RD/mks5fs.py" -s "$RDKB" -i "$RDINODES" -b 1024 -t "$STAMP" \
	-r "$RD" "$RD/root.manifest" "$B/root.img"
python3 "$RD/s5check.py" "$B/root.img" -l > "$B/root.lst" || {
	cat "$B/root.lst"; exit 1; }
tail -2 "$B/root.lst"
sha256sum "$B/root.img"
