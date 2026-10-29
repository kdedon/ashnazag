#!/bin/sh
# paula.sh -- /dev/paula in Amiberry: the module loads on first open,
# paulatest plays its scale, /dev/paula and /dev/audio exclude each
# other, the module unloads.  The sound is captured and its notes
# checked against paulatest's periods.
#
#   sh paula.sh
#
# Builds an A4000 disk with mkhdf.sh (its variables apply; the kernel is
# kernel/build/unix-amiga; UNIXBOOT and OPENPCI from toolchain/dl/amigapci)
# and boots it with amiberry.sh (KICK as there).  The guest sends its log
# to host port PORT (default 2397).  OUT: the disk directory (default
# kernel/build/amiga/paula).  Exit 0 on PASS.
set -e
A=$(cd "$(dirname "$0")/.." && pwd)
K=$(cd "$A/.." && pwd)
AUX=$(cd "$K/.." && pwd)
DL=$AUX/toolchain/dl/amigapci
OUT=${OUT:-$K/build/amiga/paula}
PORT=${PORT:-2397}
LOG=$OUT/paula.log
UNIXBOOT=${UNIXBOOT:-$DL/amix-unix-boot/src/unix_boot040}
OPENPCI=${OPENPCI:-$DL/x-openpci}
HOST=a4000
KICK=${KICK:-$AUX/images/work/amiga-stage/media/ROM/kicka4000.rom}
export UNIXBOOT OPENPCI HOST KICK
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
AUTORUN="{ modadmin -s | grep paula || echo 'paula: not loaded'
paulatest
modadmin -s | grep paula >/dev/null && echo 'paula: loaded'
sleep 6 > /dev/audio & sleep 2; paulatest; wait
: > /dev/audio && echo 'audio: opens'
modadmin -U paula && echo 'paula: unloaded'
} > /tmp/pt 2>&1; tcpcat 10.0.2.2 $PORT < /tmp/pt" \
	sh "$A/test/mkhdf.sh" "$K/build/unix-amiga" "$OUT" > "$OUT/mkhdf.log" 2>&1 ||
	{ tail -3 "$OUT/mkhdf.log"; exit 1; }
rm -f "$LOG" "$OUT/sound.raw"
python3 -I -c '
import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(1); s.settimeout(1500)
c, _ = s.accept(); f = open(sys.argv[2], "wb")
while True:
    d = c.recv(4096)
    if not d: break
    f.write(d)
' "$PORT" "$LOG" &
L=$!
MACHINE=a4000 GFX= CARDS=ne2000_pci SOUND=$OUT/sound.raw \
	sh "$A/test/amiberry.sh" "$OUT" 1500 > "$OUT/amiberry.log" 2>&1 &
W=$!
wait $L || true
kill $W 2>/dev/null || true
cat "$LOG" 2>/dev/null
# 16-bit stereo at 44100 Hz: left is channel 0's square, right channel 1's
# saw an octave lower.  Per 0.1 s, the frequency from upward crossings of
# both thresholds.  The host's pacing bends pitch, so the check is: about
# 4 s of sound, right an octave below left but where a window spans a
# note change, the last note C (period 214), about an octave above the
# first.
python3 -I -c '
import struct, sys
d = open(sys.argv[1], "rb").read()
n = len(d) // 4
s = struct.unpack("<%dh" % (2 * n), d[:4 * n])
w = 4410
def freq(x):
    lo, hi = min(x), max(x)
    if hi - lo < 2000:
        return 0
    m, h = (hi + lo) / 2, (hi - lo) * .3
    up, armed = [], False
    for i, v in enumerate(x):
        if v < m - h:
            armed = True
        elif armed and v > m + h:
            armed = False
            up.append(i)
    return 44100. * (len(up) - 1) / (up[-1] - up[0]) if len(up) > 2 else 0
f = [[freq(s[2 * i + c:2 * (i + w) + c:2]) for c in (0, 1)] for i in range(0, n - w, w)]
f = [x for x in f if x[0] and x[1]]
c3 = 3546895 / (32 * 428.)
near = lambda a, b: abs(a - b) < b * .02
first, last = sorted(f[:5])[2], sorted(f[-5:])[2]
bad = len([1 for a, b in f if not near(a, 2 * b)])
print("sound: %d windows (%d across a note change), %.0f/%.0f Hz first, %.0f/%.0f Hz last" %
    (len(f), bad, first[0], first[1], last[0], last[1]))
ok = 34 <= len(f) <= 46 and bad <= 12 and near(last[0], 2 * c3) and abs(last[0] / first[0] - 2) < .16
print("sound: " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
' "$OUT/sound.raw"
grep -q 'refused, EBUSY (right)' "$LOG" && grep -q '^/dev/paula: Device busy' "$LOG" &&
	grep -q '^paula: not loaded' "$LOG" && grep -q '^paula: loaded' "$LOG" &&
	grep -q '^audio: opens' "$LOG" && grep -q '^paula: unloaded' "$LOG"
