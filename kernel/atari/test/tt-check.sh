#!/bin/sh
# tt-check.sh -- a TT030 (10 MB ST, 16 MB TT-RAM) in Hatari boots KERNEL to
# the RAM-disk root shell and runs tt-keys.txt: uname and dstest info.
# The root is the linked-in one plus the display module, which a kernel's
# own root image cannot carry: the module is built against that kernel.
#
#   sh kernel/atari/test/tt-check.sh OUTDIR [KERNEL]
#
# KERNEL defaults to kernel/build/unix-atari030.elf.  Skips (exit 0)
# without Hatari.
set -e
T=$(cd "$(dirname "$0")" && pwd)
A=$(cd "$T/.." && pwd)
AUX=$(cd "$A/../.." && pwd)
OUT=$1 KERNEL=${2:-$AUX/kernel/build/unix-atari030.elf}
[ -n "$OUT" ] || { echo "usage: $0 OUTDIR [KERNEL]" >&2; exit 2; }
HATARI=${HATARI:-$AUX/ref/hatari/build/src/hatari}
export HATARI
[ -x "$HATARI" ] || { echo "[SKIP] no Hatari"; exit 0; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
case $KERNEL in /*) ;; *) KERNEL=$PWD/$KERNEL ;; esac

RD=$AUX/kernel/mac/ramdisk
sh "$AUX/kernel/mac/display/mods.sh" "$KERNEL" "$OUT/display" > "$OUT/display.log" 2>&1 ||
	{ cat "$OUT/display.log"; echo "[FAIL] display module"; exit 1; }
{ cat "$RD/root.manifest"
  echo 'd /etc/conf 755 0 3'
  echo 'd /etc/conf/mod.d 755 0 3'
  echo "f /etc/conf/mod.d/dsuser 644 0 3 $OUT/display/mod.d/dsuser"; } > "$OUT/root.manifest"
python3 "$RD/mks5fs.py" -s 1024 -i 256 -b 1024 -t 723000000 -r "$RD" \
	"$OUT/root.manifest" "$OUT/root.img" > /dev/null

sed "s|OUT/|$OUT/|g" "$T/tt-keys.txt" > "$OUT/keys.txt"
WAIT=${WAIT:-120} sh "$A/run-hatari.sh" -m tt -s 10 -t 16 -k "$KERNEL" -R "$OUT/root.img" \
	-a nfcons -f 400000 -T 300 -o "$OUT/final.png" -l "$OUT/tt.log" -K "$OUT/keys.txt" -- \
	--monitor vga > "$OUT/run.txt" 2>&1 || true
rm -f "$OUT/root.img"
# the key script quits Hatari itself, so there is no final screenshot
! grep -q 'key script' "$OUT/run.txt" && grep -q 'TT Shifter' "$OUT/tt.log.out" &&
	! grep -q PANIC "$OUT/tt.log.out" && [ -s "$OUT/tt-shell.png" ] && echo "[OK] tt check" || { echo "[FAIL] tt check: $OUT/tt.log.out"; exit 1; }
