#!/bin/sh
# drmkill.sh -- drmkill on the Voodoo3 in WinUAE: clients killed or
# stalled holding the DRM lock must leave the card and display usable.
#
#   sh drmkill.sh [rounds]
#
# Builds a disk with mkhdf.sh (its variables apply; the kernel is
# kernel/build/unix-amiga) whose boot runs drmkill and sends the result
# to host port PORT (default 2396), boots it with winuae.sh (KICK,
# VOODOO3, WINUAE as there) and prints the result.  Exit 0 on PASS.
# OUT: the disk directory (default kernel/build/amiga/drmkill).
set -e
A=$(cd "$(dirname "$0")/.." && pwd)
K=$(cd "$A/.." && pwd)
OUT=${OUT:-$K/build/amiga/drmkill}
PORT=${PORT:-2396}
LOG=$OUT/drmkill.log
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
sh "$A/test/winuae.sh" "$OUT" 3000 > "$OUT/winuae.log" 2>&1 &
W=$!
wait $L || true
kill $W 2>/dev/null || true
WINEPREFIX=${WINUAE:-$K/../toolchain/winuae}/prefix wineserver -k 2>/dev/null || true
cat "$LOG" 2>/dev/null
grep -q '^drmkill: PASS' "$LOG" 2>/dev/null
