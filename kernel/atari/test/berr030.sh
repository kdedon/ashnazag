#!/bin/sh
# berr030.sh -- a 68030 data-read bus error completed by its handler
# (data in the long frame's input buffer, DF cleared) must not be rerun
# by rte.  Four reads: abs.w, (An)+, (An)+ to (An)+, user mode; each
# must fault once and see the handler's data.
#
#   sh kernel/atari/test/berr030.sh OUTDIR
#
# TOSROM and HATARI as for falcon-check.sh.  Skips without them.
set -e
T=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$T/../../.." && pwd)
OUT=$1
[ -n "$OUT" ] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
HATARI=${HATARI:-$AUX/ref/hatari/build/src/hatari}
[ -x "$HATARI" ] || { echo "[SKIP] no Hatari"; exit 0; }
ROM=${TOSROM:-$(ls "$AUX"/tos404*.zip 2>/dev/null | head -1)}
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "[SKIP] no TOS 4.04 image"; exit 0; }
mkdir -p "$OUT/hd/AUTO" "$OUT/home"
OUT=$(cd "$OUT" && pwd)
case $ROM in
*.zip)	unzip -p "$ROM" '*.img' '*.IMG' '*.rom' '*.ROM' > "$OUT/tos404.img" 2>/dev/null || true
	ROM=$OUT/tos404.img ;;
esac
B=$AUX/toolchain/bin
"$B/m68k-elf-as" -m68030 -o "$OUT/berr030.o" "$T/berr030.s"
"$B/m68k-elf-objcopy" -O binary -j .text "$OUT/berr030.o" "$OUT/berr030.bin"
python3 - "$OUT/berr030.bin" "$OUT/hd/AUTO/BERR030.PRG" <<'PY'
import struct, sys
t = open(sys.argv[1], 'rb').read()
t += b'\0' * (len(t) & 1)
# no relocation: the code is position-independent
open(sys.argv[2], 'wb').write(struct.pack('>HlllllLH', 0x601a, len(t), 0, 0, 0, 0, 0, 1) + t)
PY
HOME=$OUT/home SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout -k 5 120 nice -n 19 "$HATARI" \
	--machine falcon --cpulevel 3 --fpu none --mmu on --addr24 off --memsize 4 --dsp none \
	--tos "$ROM" --monitor vga --harddrive "$OUT/hd" --natfeats yes --fast-forward on \
	--sound off --confirm-quit no > "$OUT/berr030.log" 2>&1 || true
grep -a '^t[1-4] ' "$OUT/berr030.log" || true
n=$(grep -a -c '^t[1-4] n=00000001 d=0*[12]*1234 ' "$OUT/berr030.log" || true)
[ "$n" = 4 ] && echo "[OK] berr030" || { echo "[FAIL] berr030"; exit 1; }
