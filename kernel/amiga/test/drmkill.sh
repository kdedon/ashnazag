#!/bin/sh
# drmkill.sh -- drmkill on the Voodoo3 in Amiberry: clients killed or
# stalled holding the DRM lock must leave the card and display usable.
#
#   sh drmkill.sh [rounds]
#
# Builds a disk with mkhdf.sh (its variables apply; the kernel is
# kernel/build/unix-amiga; PKG, stage.sh's outdir, gives /dev/fb0; HOST
# default a4000, ROOTBUS ncr770, UNIXBOOT
# and OPENPCI from toolchain/dl/amigapci) whose boot runs drmkill and
# sends the result to host port PORT (default 2396), boots it with
# amiberry.sh as an A4000/060 with a CyberStorm PPC, a Mediator and a
# Voodoo3 (KICK, CSPPC as there) and prints the result.  Exit 0 on PASS.
# OUT: the disk directory (default kernel/build/amiga/drmkill).
set -e
A=$(cd "$(dirname "$0")/.." && pwd)
K=$(cd "$A/.." && pwd)
AUX=$(cd "$K/.." && pwd)
DL=$AUX/toolchain/dl/amigapci
OUT=${OUT:-$K/build/amiga/drmkill}
PORT=${PORT:-2396}
LOG=$OUT/drmkill.log
UNIXBOOT=${UNIXBOOT:-$DL/amix-unix-boot/src/unix_boot040}
OPENPCI=${OPENPCI:-$DL/x-openpci}
HOST=${HOST:-a4000}
ROOTBUS=${ROOTBUS-ncr770}
KICK=${KICK:-$AUX/images/work/amiga-stage/media/ROM/kicka4000.rom}
CSPPC=${CSPPC:-$AUX/media/cyberstormppc.rom}
export UNIXBOOT OPENPCI HOST ROOTBUS KICK CSPPC
mkdir -p "$OUT"
AUTORUN="drmkill ${1:-10} > /tmp/dk 2>&1; tcpcat 10.0.2.2 $PORT < /tmp/dk" \
	sh "$A/test/mkhdf.sh" "$K/build/unix-amiga" "$OUT" > "$OUT/mkhdf.log" 2>&1 ||
	{ tail -3 "$OUT/mkhdf.log"; exit 1; }
rm -f "$LOG"
python3 -I -c '
import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(1); s.settimeout(3000)
c, _ = s.accept(); f = open(sys.argv[2], "wb")
while True:
    d = c.recv(4096)
    if not d: break
    f.write(d)
' "$PORT" "$LOG" &
L=$!
MACHINE=a4000 CPU=68060 MEDWIN=512 GFX=VOODOO3 CARDS=ne2000_pci \
	sh "$A/test/amiberry.sh" "$OUT" 3000 > "$OUT/amiberry.log" 2>&1 &
W=$!
wait $L || true
kill $W 2>/dev/null || true
cat "$LOG" 2>/dev/null
grep -q '^drmkill: PASS' "$LOG" 2>/dev/null
