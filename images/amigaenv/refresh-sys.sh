#!/bin/sh
# refresh-sys.sh sysdir -- the environment's own files in a copy of the
# staged SYS:, built from this tree: the card, its Picasso96 settings, the
# startup, input, session, network, Warp3D and sound drivers.  A staged
# card older than the launcher fails its header check and never binds.
set -e
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
K=$AUX/kernel/guest/amiga
A=$AUX/tools/amiga
SYS=$(cd "$1" && pwd)
O=$SYS.build
rm -rf "$O"
mkdir -p "$O"
sh "$K/rtg/build.sh" "$O/rtg" > /dev/null
sh "$K/input/build.sh" "$O/input" > /dev/null
sh "$K/session/build.sh" "$O/session" > /dev/null
sh "$K/bsdsock/build.sh" "$O/bsdsock" > /dev/null
sh "$K/warp3d/build.sh" "$O/warp3d" > /dev/null
sh "$K/ahi/build.sh" "$O/ahi" > /dev/null
python3 -c 'import sys; sys.path[0] = sys.argv[1]; from template import STARTUP; print(STARTUP, end="")' \
    "$A" > "$O/Startup-Sequence"
DEF=$(cd "$SYS" && find . -ipath './prefs/env-archive/sys/def_tool.info' | head -1)
python3 -c 'import sys; sys.path[0] = sys.argv[1]; from template import donotwait
sys.stdout.buffer.write(donotwait(open(sys.argv[2], "rb").read()))' "$A" "$SYS/$DEF" > "$O/Session.info"
{
	python3 "$A/p96prefs.py" "$O/p96" | sed "s#.*#&	$O/p96/&#"
	python3 "$A/ahiprefs.py" "$O/ahiprefs" | sed "s#.*#&	$O/ahiprefs/&#"
	printf '%s\t%s\n' Libs/Picasso96/container.card "$O/rtg/container.card" \
	    S/Startup-Sequence "$O/Startup-Sequence" \
	    C/container-input "$O/input/container-input" \
	    WBStartup/Session "$O/session/Session" \
	    WBStartup/Session.info "$O/Session.info" \
	    Libs/bsdsocket.library "$O/bsdsock/bsdsocket.library" \
	    Libs/Warp3D.library "$O/warp3d/Warp3D.library" \
	    Devs/AHI/container.audio "$O/ahi/container.audio"
} | python3 -c '
import os, shutil, sys
root = sys.argv[1]
for line in sys.stdin:
    rel, src = line.rstrip("\n").split("\t")
    cur = root
    parts = rel.split("/")
    # SYS: names are case-insensitive: an existing entry of any case is the one
    for i, part in enumerate(parts):
        names = {n.lower(): n for n in os.listdir(cur)}
        cur = os.path.join(cur, names.get(part.lower(), part))
        if i < len(parts) - 1 and not os.path.isdir(cur):
            os.mkdir(cur)
    shutil.copyfile(src, cur)
' "$SYS"
rm -rf "$O"
