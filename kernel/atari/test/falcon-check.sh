#!/bin/sh
# falcon-check.sh -- the Falcon disk image on TOS 4.04 in Hatari: power-on
# boot to a login on a VGA and an RGB monitor, root and guest logins,
# ps -ef, a display session hidden and shown with the hot key, then
# the guest modules loaded and a restart, the TOS environment on the
# machine's ROM: its GEM desktop, the hot key to the console and back,
# and Ctrl-C back to the shell.
#
#   sh kernel/atari/test/falcon-check.sh OUTDIR [IMAGE]
#
# IMAGE defaults to kernel/build/atari/disk/falcon-disk.img; it is copied.
# TOSROM: the TOS 4.04 image or its zip (default: tos404*.zip in the
# repo root).  Skips (exit 0) without the ROM or Hatari.
# STAGES: any of vga rgb mod tos tosloop (mod, tos and tosloop need an
# image made with TESTS=modadmin MODS=<mod.d>) (default "vga rgb").
# tosloop starts the TOS environment BOOTS times (default 15) in one
# session and checks each desktop.
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
	# tosloop writes its own
	[ ! -f "$T/falcon-$1-keys.txt" ] || sed "s|OUT/|$OUT/$1-|" "$T/falcon-$1-keys.txt" > "$OUT/$1-keys.txt"
	WAIT=${WAIT:-600} sh "$A/run-hatari.sh" -r "$ROM" -d "$OUT/disk.img" -f 400000 -T ${3:-1500} \
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
		check "$OUT/tos.log.out" 'auxreg-rc=0' 'mods-done' '\^C' 'log-done' 'starttos-left=0'
		# the desktop, the console on the hot key, the desktop again, the shell after
		# the console's Ctrl-C
		python3 "$T/tosshot.py" desktop "$OUT/tos-1-desktop.png" || fail=1
		# Ctrl-C in front is a key for TOS, not SIGINT: the session survives it
		python3 "$T/tosshot.py" differ "$OUT/tos-1-ctrlc.png" "$OUT/tos-2-console.png" || fail=1
		python3 "$T/tosshot.py" same "$OUT/tos-1-ctrlc.png" "$OUT/tos-3-back.png" || fail=1
		python3 "$T/tosshot.py" differ "$OUT/tos-3-back.png" "$OUT/tos-4-exit.png" || fail=1 ;;
	tosloop)
		n=${BOOTS:-15}
		{ sed -n '1,/^wait mods-done/p' "$T/falcon-tos-keys.txt"
		i=1; while [ $i -le $n ]; do
			printf '%s\n' 'type /tests/starttos -P -C /tmp/c -c /tests/tosml.img -v 2> /tmp/st.log\n' 'sleep 120'
			echo "debug screenshot $OUT/tosloop-$i.png"
			# the hot key to the console, then Ctrl-C there ends the session
			printf 'event keydown 0x1d\nevent keydown 0x38\nevent keypress 0x01\nevent keyup 0x38\nevent keyup 0x1d\nsleep 5\n'
			printf 'event keydown 0x1d\nevent keypress 0x2e\nevent keyup 0x1d\nwait ^C\n'
			printf 'type tail -1 /tmp/st.log; echo boot-%s-done\\n\nwait boot-%s-done\n' $i $i
			i=$((i + 1))
		done
		echo "debug quit 0"; } > "$OUT/tosloop-keys.txt"
		run tosloop vga $((n * 200 + 600))
		i=1; while [ $i -le $n ]; do
			python3 "$T/tosshot.py" desktop "$OUT/tosloop-$i.png" || fail=1
			i=$((i + 1))
		done ;;
	esac
done
[ $fail -eq 0 ] && echo "[OK] falcon check" || { echo "[FAIL] falcon check"; exit 1; }
