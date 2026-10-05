#!/bin/sh
# falcon-check.sh -- the Falcon disk image on TOS 4.04 in Hatari: power-on
# boot to a login on a VGA and an RGB monitor, root and guest logins,
# ps -ef, a display session hidden and shown with the hot key, then
# the guest modules loaded and a restart, the TOS environment on the
# machine's ROM.
#
#   sh kernel/atari/test/falcon-check.sh OUTDIR [IMAGE]
#
# IMAGE defaults to kernel/build/atari/disk/falcon-disk.img; it is copied.
# TOSROM: the TOS 4.04 image or its zip (default: tos404*.zip in the
# repo root).  Skips (exit 0) without the ROM or Hatari.
# STAGES: any of vga rgb mod tos (mod needs an image made with
# TESTS=modadmin MODS=<mod.d>) (default "vga rgb").
set -e
T=$(cd "$(dirname "$0")" && pwd)
A=$(cd "$T/.." && pwd)
AUX=$(cd "$A/../.." && pwd)
OUT=$1 IMG=${2:-$AUX/kernel/build/atari/disk/falcon-disk.img}
STAGES=${STAGES:-vga rgb}
[ -n "$OUT" ] || { echo "usage: $0 OUTDIR [IMAGE]" >&2; exit 2; }
HATARI=${HATARI:-$AUX/ref/hatari/build/src/hatari}
[ -x "$HATARI" ] || { echo "[SKIP] no Hatari"; exit 0; }
ROM=${TOSROM:-$(ls "$AUX"/tos404*.zip 2>/dev/null | head -1)}
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "[SKIP] no TOS 4.04 image"; exit 0; }
[ -f "$IMG" ] || { echo "[FAIL] no image $IMG"; exit 1; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
case $ROM in
*.zip)	unzip -p "$ROM" '*.img' '*.IMG' '*.rom' '*.ROM' > "$OUT/tos404.img" 2>/dev/null || true
	ROM=$OUT/tos404.img ;;
esac
[ $(wc -c < "$ROM") -eq 524288 ] || { echo "[FAIL] $ROM: not a 512 KB TOS image"; exit 1; }

B=$AUX/kernel/build/atari/disk/boot

fail=0
check() {	# log pattern...
	l=$1; shift
	for s; do
		if grep -q -- "$s" "$l"; then echo "[OK] $s"; else echo "[FAIL] $s"; fail=1; fi
	done
}
run() {		# stage monitor
	# a fresh copy per stage, whose kernel also copies the console to the log
	cp --sparse=always "$IMG" "$OUT/disk.img"
	python3 "$A/instboot.py" "$OUT/disk.img" "$B/bootsec.bin" "$B/axbload.bin" \
		"${KERNEL:-$AUX/kernel/build/unix-atari030.elf}" "root=c?d0s1 nfcons" > /dev/null
	sed "s|OUT/|$OUT/$1-|" "$T/falcon-$1-keys.txt" > "$OUT/$1-keys.txt"
	WAIT=${WAIT:-600} sh "$A/run-hatari.sh" -r "$ROM" -d "$OUT/disk.img" -f 400000 -T 1500 \
		-o "$OUT/$1-final.png" -l "$OUT/$1.log" -K "$OUT/$1-keys.txt" -- \
		--natfeats yes --monitor "$2" > "$OUT/$1-run.txt" 2>&1 || true
	if grep -q 'key script' "$OUT/$1-run.txt"; then echo "[FAIL] $1: key script"; fail=1; fi
}
for s in $STAGES; do
	case $s in
	vga)	run vga vga
		check "$OUT/vga.log.out" 'login:' 'uname=' 'ps-lines= *[1-9]' 'proc on /proc' \
			'session 1 hidden, serial' 'session 1 shown, serial' 'guest-id=uid=100' ;;
	rgb)	run rgb rgb
		check "$OUT/rgb.log.out" 'login:' 'uname=' 'ps-lines= *[1-9]' ;;
	mod)	run mod vga
		check "$OUT/mod.log.out" 'auxreg-rc=0' 'opened-ok' "$(printf '\ttosguest')" "$(printf '\tuinter')" 'mods-done' 'stat-rc=0' 'rebooted-ok'
		! grep -q '^modfail=\|PANIC\|panic' "$OUT/mod.log.out" || { echo "[FAIL] module load"; fail=1; } ;;
	tos)	run tos vga
		check "$OUT/tos.log.out" 'tosguest' 'starttos-rc=0' ;;
	esac
done
[ $fail -eq 0 ] && echo "[OK] falcon check" || { echo "[FAIL] falcon check"; exit 1; }
