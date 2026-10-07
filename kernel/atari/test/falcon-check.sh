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
# STAGES: any of vga rgb mod tos tosloop x xtos xdm xdmboots mac6 (mod, tos and tosloop need an
# image made with TESTS=modadmin MODS=<mod.d>, tos also takes TOSENV=1; x one made with X11=<pkg>)
# (default "vga rgb").  x: startx in 256 colours, typing into xterm, the
# server stopped and the console back; then in 2 colours with the German
# layout (x-4-de.png shows "keyz"), stopped from the console.
# tosloop starts the TOS environment BOOTS times (default 15) in one
# session and checks each desktop.  xtos (an image made with X11=<pkg>
# TOSENV=1): guest starts X, then starttos -P from the console; the hot
# keys switch between X, the GEM desktop and the console; each ends.
# xdm (an image made with X11=<pkg> TOSENV=1 BOOTX=1 XPKGS=xview): guest
# logs in on xdm and picks Console, twm, XView and TOS in turn, each
# ending back at the login; Ctrl-C goes to TOS, Ctrl-C on the console
# ends it; root lists the sessions; 2 colours with German keys; TOS last,
# Ctrl-C to TOS, Ctrl-C on the console ends it and the login comes back.
# xdmboots (the same image): BOOTS (default 5) boots, each to the xdm
# login without a crash; root logs in and restarts the machine.
# mac6 (an image made with MACENV=1): System 6's Finder desktop; Command-A
# and Command-O open its windows; the hot key to the console, whose
# Ctrl-C ends it.
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
	[ ! -f "$T/falcon-$1-keys.txt" ] || sed "s|OUT/|$OUT/$1-|g" "$T/falcon-$1-keys.txt" > "$OUT/$1-keys.txt"
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
	x)	run x vga
		check "$OUT/x.log.out" 'x-2-up' 'key-42-ok' 'x-7-ended' 'fb: /dev/fb0 Videl 640x480 depth 8' \
			'x-4-up' 'x-11-ended' 'fb: /dev/fb0 Videl 640x480 depth 1' 'x-9-done'
		! grep -q 'Fatal\|PANIC\|panic' "$OUT/x.log.out" || { echo "[FAIL] x: server or kernel error"; fail=1; } ;;
	xtos)	run xtos vga
		check "$OUT/xtos.log.out" 'x-2-up' 'tos-left=0' 'x-7-ended' 'xtos-9-done'
		! grep -q 'Fatal\|PANIC\|panic' "$OUT/xtos.log.out" || { echo "[FAIL] xtos: server or kernel error"; fail=1; }
		python3 "$T/tosshot.py" desktop "$OUT/xtos-3-tos.png" || fail=1
		python3 "$T/tosshot.py" same "$OUT/xtos-1-x.png" "$OUT/xtos-4-x.png" || fail=1
		python3 "$T/tosshot.py" same "$OUT/xtos-3-tos.png" "$OUT/xtos-5-tos.png" || fail=1 ;;
	xdm)	run xdm vga 2400
		check "$OUT/xdm.log.out" 'xdm-9-done' 'xsession console' 'xsession twm' 'xsession xview' \
			'fb: /dev/fb0 Videl 640x480 depth 8'
		! grep -q 'BUS ERROR\|Fatal\|PANIC\|panic' "$OUT/xdm.log.out" || { echo "[FAIL] xdm: client, server or kernel error"; fail=1; }
		python3 "$T/tosshot.py" desktop "$OUT/xdm-9-tos.png" || fail=1
		# Ctrl-C is a key for TOS: the session stays until the console's Ctrl-C
		python3 "$T/tosshot.py" differ "$OUT/xdm-11-console.png" "$OUT/xdm-10-tos-ctrlc.png" || fail=1
		# the login is back after TOS ends
		python3 "$T/tosshot.py" same "$OUT/xdm-7-mono-greeter.png" "$OUT/xdm-12-greeter.png" || fail=1 ;;
	xdmboots)
		n=${BOOTS:-5}
		{ i=1; while [ $i -le $n ]; do
			printf 'wait The system is ready.\nsleep 90\ndebug screenshot %s\n' "$OUT/xdmboots-$i.png"
			printf 'type root\\n\nsleep 3\ntype \\n\n'
			j=0; while [ $j -lt 12 ]; do printf 'sleep 1.5\ntype 4\n'; j=$((j + 1)); done
			printf 'sleep 30\ntype \\necho xdmboot-`expr %s + 0`-up > /dev/console\\n\nwait xdmboot-%s-up\n' $i $i
			[ $i -lt $n ] && printf '%s\n' 'type /etc/init 6\n'
			i=$((i + 1))
		done
		echo "debug quit 0"; } > "$OUT/xdmboots-keys.txt"
		run xdmboots vga $((n * 400 + 300))
		i=1; while [ $i -le $n ]; do
			check "$OUT/xdmboots.log.out" "xdmboot-$i-up"
			i=$((i + 1))
		done
		! grep -q 'BUS ERROR\|PANIC\|panic' "$OUT/xdmboots.log.out" || { echo "[FAIL] xdmboots: client or kernel error"; fail=1; } ;;
	mac6)	run mac6 vga
		check "$OUT/mac6.log.out" 'uinter: ROM /etc/aux/rom, 512 KB, version 67C' 'startmac-left=0'
		! grep -q 'BUS ERROR\|PANIC\|panic' "$OUT/mac6.log.out" || { echo "[FAIL] mac6: fault notice or kernel error"; fail=1; }
		python3 "$T/tosshot.py" differ "$OUT/mac6-1-desktop.png" "$OUT/mac6-2-open.png" || fail=1
		python3 "$T/tosshot.py" differ "$OUT/mac6-2-open.png" "$OUT/mac6-3-console.png" || fail=1 ;;
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
