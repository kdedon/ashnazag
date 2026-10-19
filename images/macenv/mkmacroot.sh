#!/bin/sh
# mkmacroot.sh -- A/UX's Mac files as A/UX installs them, with Mac OS
# 7.6.1 as root's System Folder and the users' template.
#
#   sh images/macenv/mkmacroot.sh auxroot cd761.iso outdir
#
# Proprietary: the copy stays local.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
AUXROOT=$1
CD761=$2
M=$3
C=$M.cc
rm -rf "$M" "$C"
mkdir -p "$M/usr/bin" "$M/usr/lib" "$M/.mac/localhost/Desktop Folder" "$M/.mac/localhost/Trash"
(cd "$AUXROOT" && tar cf - mac " Applications" " Documentation" " Shared Data" \
	" System Folder alias") | (cd "$M" && tar xf -)
COFFRSRC="$AUXROOT/mac/bin/%CommandShell" sh "$AUX/tests/aux/coffcarrier/build.sh" "$C"
python3 "$AUX/tests/aux/coffcarrier/install.py" "$AUXROOT/mac/bin/CommandShell" \
    "$AUXROOT/mac/bin/%CommandShell" "$C/COFFCarrier" "$M"
rm -rf "$C"
cp "$AUXROOT/usr/bin/systemfolder" "$M/usr/bin/"
cp "$AUXROOT/usr/lib/updtsysfldr" "$M/usr/lib/"
find "$M" -name '.fs_*' -o -name '%.fs_*' | while read f; do rm -f "$f"; done
# Shut Down's dialog (root's only with other users logged in): Cancel ends
# the session, so it reads Logout
python3 -c "import sys; sys.path.insert(0, sys.argv[1]); import rsrcedit
f = sys.argv[2]; d = bytearray(open(f, 'rb').read())
for i in (128, 129, 130):
	o, l, a = rsrcedit.resource(d, b'DITL', i)
	k = d.index(b'\x04\x06Cancel', o, o + l)
	d[k + 2:k + 8] = b'Logout'
open(f, 'wb').write(d)" "$AUX/tools" "$M/mac/lib/Resources/%AUX Resources"
# Mac OS 7.6.1 in place of A/UX's System 7.0.1: the template, and
# root's copy
rm -rf "$M/mac/sys/System Folder"
sh "$D/mksys76.sh" "$CD761" "$M/mac/lib/System Folder" "$AUXROOT"
cp -rp "$M/mac/lib/System Folder" "$M/mac/sys/"
# users' Special menu: Log Out in place of Restart and Shut Down
python3 "$D/userfinder.py" "$M/mac/lib/System Folder/%Finder"
