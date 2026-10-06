#!/bin/sh
# mksys6.sh -- the System 6 Mac environment of A/UX 2.0.1 from its CD
# image: a root to run it in (chroot) with A/UX 2.0.1's startmac and
# shared libraries, and System 6.0.7 with Finder 6.1.7 and MultiFinder
# in /mac/sys/System Folder (AppleSingle files, as A/UX 2 keeps them).
# A/UX 2's volumes report a desktop database, so the Finder needs the
# Desktop Manager, _DTInit.
#
#   sh images/macenv/mksys6.sh cd.iso outdir
#
# The CD is proprietary; the copy stays local.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
ISO=$1
OUT=$2
[ -f "$ISO" ] && [ -n "$OUT" ] || { echo "usage: mksys6.sh cd.iso outdir"; exit 2; }
case $OUT in /|"$HOME"|"$HOME"/) echo "mksys6.sh: refusing $OUT"; exit 2;; esac
rm -rf "$OUT"
mkdir -p "$OUT"
python3 - "$ISO" "$OUT" "$AUX/tools" <<'EOF'
import sys, os, struct
iso, out, tools = sys.argv[1:4]
sys.path.insert(0, tools)
from ufs import UFS

# the A/UX root: the first Apple_UNIX_SVR2 partition, a UFS
f = open(iso, 'rb')
start = None
for i in range(1, 64):
    f.seek(512 * i)
    b = f.read(512)
    if b[:2] != b'PM':
        break
    if b[48:63] == b'Apple_UNIX_SVR2':
        start = struct.unpack('>I', b[8:12])[0]
        break
if start is None:
    sys.exit('mksys6.sh: no A/UX partition on ' + iso)

class Part(UFS):
    def rd(s, off, n):
        s.f.seek(512 * start + off)
        return s.f.read(n)
u = Part(iso)

def look(path):
    n = 2
    for c in path.strip('/').split('/'):
        n = dict(u.readdir(n))[c]
    return n

def get(src, dst):
    i = u.inode(look(src))
    if i['mode'] & 0o170000 == 0o120000:		# shared items are symlinks
        return get(u.read(i).decode('latin1'), dst)
    p = os.path.join(out, dst.strip('/'))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(u.read(i))
    os.chmod(p, 0o755 if i['mode'] & 0o111 else 0o644)

for p in ('/mac/bin/startmac', '/shlib/libc1_s', '/shlib/libmac1_s'):
    get(p, p)
sf = '/mac/sys/System Folder'
for n in ('System', 'Finder', 'MultiFinder', 'DA Handler', '%AUX Resources',
          'Patch.067C', 'Patch.0178', 'General', 'Keyboard', 'Key Layout', 'Mouse',
          'Color', 'Monitors', 'Sound', '+Layers', '+NMgrFix', 'Scrapbook File', '_DTInit'):
    get(sf + '/' + n, sf + '/' + n)
EOF
# Log Out in the Apple menu, a desk accessory in the System file
sh "$D/logout/build.sh" "$OUT/tmp/logout" "$OUT/mac/sys/System Folder/System"
rm -rf "$OUT/tmp/logout"
mkdir -p "$OUT/tmp" "$OUT/etc"
echo "root::0:0:root:/:/bin/sh" > "$OUT/etc/passwd"
