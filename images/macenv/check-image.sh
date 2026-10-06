#!/bin/sh
# check-image.sh -- boot a built disk image and check each environment
# from a guest login: starttos reaches the desktop, startmig Workbench,
# startmac the Finder.
#
#   sh images/macenv/check-image.sh [image [outdir]]
#
# The image (default images/q800-unix-disk.img) is copied into outdir
# (default images/work/check-image) and booted from the Quadra 800 ROM
# (ROM=), 128 MB, 640x480x8.  Screenshots and check.txt go in outdir.
# XDM=1 for an image that boots to xdm.
# A screen passes when it shows more colours than the console's two.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
IMG=${1:-$AUX/images/q800-unix-disk.img}
OUT=${2:-$AUX/images/work/check-image}
ROM=${ROM:-$AUX/Quadra 800.ROM}
[ -f "$IMG" ] || { echo "[FAIL] no image $IMG"; exit 1; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
rm -f "$OUT"/*.png "$OUT/check.txt"
cp --sparse=always "$IMG" "$OUT/disk.img"
HOT='hmp:sendkey ctrl-alt-meta_l-0'
# XDM=1: the image boots to xdm (BOOTX=1); each environment is chosen at
# the session chooser, whose numbers shift down by one without XView
# (XVIEW=0).  Control-C on the console returns to xdm.
# the Mac goes last: a signal from the keyboard is the Mac's
if [ -n "$XDM" ]; then
	n=3; [ "${XVIEW-1}" = 0 ] && n=2
	MAC=$n TOS=$((n + 1)) MIG=$((n + 2))
	set -- wait:150 shot:01-login \
		'type:guest\n' wait:2 'type:\n' wait:5 "type:$TOS" wait:90 shot:02-tos "$HOT" wait:5 \
		'hmp:sendkey ctrl-c' wait:25 \
		'type:guest\n' wait:2 'type:\n' wait:5 "type:$MIG" wait:30 shot:03-mig-starting wait:150 \
		shot:04-mig "$HOT" wait:5 shot:05-mig-console 'hmp:sendkey ctrl-c' wait:25 \
		'type:guest\n' wait:2 'type:\n' wait:5 "type:$MAC" wait:150 shot:07-mac quit
else
	set -- wait:150 shot:01-login 'type:guest\n' wait:5 'type:\n' wait:10 \
		'type:starttos\n' wait:90 shot:02-tos "$HOT" wait:5 'hmp:sendkey ctrl-c' wait:20 \
		'type:clear; makeamiga; startmig\n' wait:30 shot:03-mig-starting wait:150 shot:04-mig \
		"$HOT" wait:5 shot:05-mig-console 'hmp:sendkey ctrl-c' wait:15 \
		'type:clear; grep -c open Amiga/.startmig.log; grep RTG Amiga/.startmig.log\n' wait:8 shot:06-mig-log \
		'type:startmac\n' wait:150 shot:07-mac quit
fi
QEXTRA="-g 640x480x8" sh "$AUX/images/qemu/run-mac.sh" --tmo 1100 "$ROM" "$OUT/disk.img" 128 "$OUT" \
	"$@" > "$OUT/run.log" 2>&1 || :
rm -f "$OUT/disk.img"
r=0
python3 - "$OUT" > "$OUT/check.txt" <<'EOF' || r=1
import os, struct, sys, zlib
out = sys.argv[1]
def colours(name):
    p = os.path.join(out, name + '.png')
    if not os.path.exists(p):
        return 0
    d = open(p, 'rb').read()
    w, h = struct.unpack('>II', d[16:24])
    i, idat = 8, b''
    while i < len(d):
        n, = struct.unpack('>I', d[i:i + 4])
        if d[i + 4:i + 8] == b'IDAT':
            idat += d[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    seen = set()
    for y in range(0, h, 4):
        row = raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)]
        for x in range(0, w * 3, 12):
            seen.add(row[x:x + 3])
    return len(seen)
fail = 0
for name, what in (('02-tos', 'starttos: desktop'), ('04-mig', 'startmig: Workbench'),
                   ('07-mac', 'startmac: Finder')):
    n = colours(name)
    ok = n > 2
    fail += not ok
    print('%s %s (%d colours, %s.png)' % ('[ok]' if ok else '[FAIL]', what, n, name))
sys.exit(1 if fail else 0)
EOF
cat "$OUT/check.txt"
exit $r
