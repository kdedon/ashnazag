#!/bin/sh
# falcon-check.sh -- the Falcon disk image on TOS 4.04 in Hatari: power-on
# boot to a login on a VGA and an RGB monitor, root and guest logins,
# ps -ef, a display session hidden and shown with the hot key, then
# the guest modules loaded and a restart, the TOS environment on the
# machine's ROM: its GEM desktop in the console's mode, a window redrawn
# and timed, mouse moves, the hot key to the console and back, and Ctrl-C
# back to the shell.
#
#   sh kernel/atari/test/falcon-check.sh OUTDIR [IMAGE]
#
# IMAGE defaults to kernel/build/atari/disk/falcon-disk.img; it is copied.
# BOOTMODS: the boot modules, as for mkdisk.sh.
# TOSROM: the TOS 4.04 image or its zip (default: tos404*.zip in the
# repo root).  Skips (exit 0) without the ROM or Hatari.  HOPTS: more
# run-hatari.sh options, e.g. "-c 060 -t 32" for a CT60 with 32 MB FastRAM.
# STAGES: any of vga rgb emutos scsi emutos-scsi mod tos tosint tosloop cubase x xtos xdm xdmboots mac6 xaaes (mod, tos and tosloop need an
# image made with TESTS=modadmin MODS=<mod.d>, tos also takes TOSENV=1; x one made with X11=<pkg>)
# (default "vga rgb").  x: startx in 256 colours, typing into xterm, the
# server stopped and the console back; then in 2 colours with the German
# layout (x-4-de.png shows "keyz"), stopped from the console.
# tosloop starts the TOS environment BOOTS times (default 15) in one
# session and checks each desktop.  tosint: three starts in a row, each
# ended by SIGINT, each reaching the desktop.  xtos (an image made with X11=<pkg>
# TOSENV=1): guest starts X, then starttos from the console; the hot
# keys switch between X, the GEM desktop and the console; each ends.
# xdm (an image made with X11=<pkg> TOSENV=1 BOOTX=1 XPKGS=xview): guest
# logs in on xdm and picks Console, twm, XView and TOS in turn, each
# ending back at the login; Ctrl-C goes to TOS, Ctrl-C on the console
# ends it; root lists the sessions; 2 colours with German keys; TOS last,
# Ctrl-C to TOS, Ctrl-C on the console ends it and the login comes back.
# xdmboots (the same image): BOOTS (default 5) boots, each to the xdm
# login without a crash; root logs in and restarts the machine.
# cubase (an image made with TOSENV=1 TESTS=tosstat and mkcubase.sh's Cubase):
# Cubase 3.1 started CUBOOTS times (default 5) in mono, each to its Arrange
# window within CUBWAIT seconds (default 900), no hardware access after;
# skips (exit 0) when the image holds no Cubase.
# xaaes (an image made with TOSENV=1 TESTS=t_xaaes TESTROOT=tests/build/tos/root):
# t_xaaes as root, on EmuTOS: the checks needing no host line.
# mac6 (an image made with MACENV=1): System 6's Finder desktop; Command-A
# and Command-O open its windows; the hot key to the console, whose
# Ctrl-C ends it.
# emutos: power-on boot on EmuTOS (no root-sector boot; the AUTO
# launcher starts the loader) to a root login, root and /home on IDE.
# scsi, emutos-scsi: the same on SCSI ID 0, on TOS 4.04 and on EmuTOS.
# CPU=060: a CT60 Falcon (68060, TTRAM MB of FastRAM, default 64) on
# unix-atari060.elf.
set -e
T=$(cd "$(dirname "$0")" && pwd)
A=$(cd "$T/.." && pwd)
AUX=$(cd "$A/../.." && pwd)
OUT=$1 IMG=${2:-$AUX/kernel/build/atari/disk/falcon-disk.img}
STAGES=${STAGES:-vga rgb}
CPU=${CPU:-030} CPUOPT=
[ $CPU = 030 ] || CPUOPT="-t ${TTRAM:-64}"
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
BM=
for m in ${BOOTMODS-ataide scsi sd}; do BM="$BM $AUX/kernel/build/atari/disk/src/build/mods/mod.d/$m"; done

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
		"${KERNEL:-$AUX/kernel/build/unix-atari$CPU.elf}" "root=c?d0s1 nfcons" 3 $BM > /dev/null
	# tosloop writes its own
	[ ! -f "$T/falcon-$1-keys.txt" ] || sed "s|OUT/|$OUT/$1-|g" "$T/falcon-$1-keys.txt" > "$OUT/$1-keys.txt"
	WAIT=${WAIT:-600} sh "$A/run-hatari.sh" -c $CPU $CPUOPT $HOPTS -r "${R-$ROM}" -d "$OUT/disk.img" -b ${BUS:-ide} -f ${FRAMES:-400000} -T ${3:-1500} \
		-o "$OUT/$1-final.png" -l "$OUT/$1.log" -K "$OUT/$1-keys.txt" -- \
		--natfeats yes --monitor "$2" > "$OUT/$1-run.txt" 2>&1 || true
	if grep -q 'key script' "$OUT/$1-run.txt"; then echo "[FAIL] $1: key script"; fail=1; fi
}
for s in $STAGES; do
	case $s in
	vga)	run vga vga
		check "$OUT/vga.log.out" 'login:' 'uname=' 'ps-lines= *[1-9]' 'proc on /proc' \
			'session 1 hidden, serial' 'session 1 shown, serial' 'guest-id=uid=100' ;;
	emutos|scsi|emutos-scsi)
		# multi-user login, root and /home from the boot disk
		c=c0; case $s in *scsi) c=c8 ;; esac
		printf '%s\n' 'wait login:' 'type root\n' 'wait # ' \
			'type echo disk-root=`/etc/devnm /` home=`/etc/devnm /home`\n' \
			'wait home=/dev' 'debug quit 0' > "$OUT/$s-keys.txt"
		case $s in emutos*) R= ;; esac
		case $s in *scsi) BUS=scsi ;; esac
		run $s vga; unset R BUS
		check "$OUT/$s.log.out" 'The system is ready' "disk-root=/dev/dsk/${c}d0s1 / home=/dev/dsk/${c}d0s3 /home" ;;
	rgb)	run rgb rgb
		check "$OUT/rgb.log.out" 'login:' 'uname=' 'ps-lines= *[1-9]' ;;
	mod)	run mod vga
		check "$OUT/mod.log.out" 'auxreg-rc=0' 'opened-ok' "$(printf '\ttosguest')" "$(printf '\tuinter')" 'mods-done' 'stat-rc=0' 'rebooted-ok'
		! grep -q '^modfail=\|PANIC\|panic' "$OUT/mod.log.out" || { echo "[FAIL] module load"; fail=1; } ;;
	tos)	run tos vga
		check "$OUT/tos.log.out" 'auxreg-rc=0' 'mods-done' '\^C' 'log-done' 'starttos-left=0' 'xaaes-log=1'
		# the 640x480 1-bit console: TOS in ST high, not TT high
		m=$(grep -a 'starttos: screen mode' "$OUT/tos.log.out" | tail -1)
		case $m in *'mode 200 '*) echo "[OK] $m" ;; *) echo "[FAIL] TOS not in ST high: $m"; fail=1 ;; esac
		# the desktop, the console on the hot key, the desktop again, the shell after
		# the console's Ctrl-C
		python3 "$T/tosshot.py" desktop "$OUT/tos-1-desktop.png" || fail=1
		# Ctrl-C in front is a key for TOS, not SIGINT: the session survives it
		python3 "$T/tosshot.py" differ "$OUT/tos-1-ctrlc.png" "$OUT/tos-2-console.png" || fail=1
		python3 "$T/tosshot.py" same "$OUT/tos-1-ctrlc.png" "$OUT/tos-3-back.png" || fail=1
		python3 "$T/tosshot.py" differ "$OUT/tos-3-back.png" "$OUT/tos-4-exit.png" || fail=1
		python3 "$T/tosshot.py" desktop "$OUT/tos-5-xaaes.png" || fail=1
		# the console's size; after boot no hardware access or emulation
		python3 "$T/tossteady.py" "$OUT/tos-1-desktop.png" "$OUT/tos-2-console.png" "$OUT/tos.log.out" || fail=1 ;;
	x)	run x vga
		check "$OUT/x.log.out" 'x-2-up' 'key-42-ok' 'x-7-ended' 'fb: /dev/fb0 Videl 640x480 depth 8' \
			'x-4-up' 'keyz' 'x-11-ended' 'fb: /dev/fb0 Videl 640x480 depth 1' 'x-9-done'
		! grep -q 'BUS ERROR\|Fatal\|PANIC\|panic' "$OUT/x.log.out" || { echo "[FAIL] x: client, server or kernel error"; fail=1; } ;;
	xtos)	run xtos vga
		check "$OUT/xtos.log.out" 'x-2-up' 'typed-in-xterm' 'tos-left=0' 'x-7-ended' 'xtos-9-done'
		! grep -q 'BUS ERROR\|Fatal\|PANIC\|panic' "$OUT/xtos.log.out" || { echo "[FAIL] xtos: client, server or kernel error"; fail=1; }
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
	xaaes)	run xaaes vga 3000
		check "$OUT/xaaes.log.out" 'PASS xaaes.preempt' 'PASS xaaes.pfork' 'xaaes-rc=0'
		! grep -q 'FAIL xaaes\|PANIC\|panic' "$OUT/xaaes.log.out" || { echo "[FAIL] xaaes"; fail=1; } ;;
	mac6)	run mac6 vga
		check "$OUT/mac6.log.out" 'ROMSize  *= 0x80000' 'ROMVersion  *= 0x67c' 'startmac-left=0'
		! grep -q 'BUS ERROR\|PANIC\|panic' "$OUT/mac6.log.out" || { echo "[FAIL] mac6: fault notice or kernel error"; fail=1; }
		python3 "$T/tosshot.py" differ "$OUT/mac6-1-desktop.png" "$OUT/mac6-2-open.png" || fail=1
		python3 "$T/tosshot.py" differ "$OUT/mac6-2-open.png" "$OUT/mac6-3-console.png" || fail=1 ;;
	tosint)
		# each start must find its ST-RAM again after the last one's SIGINT
		{ sed -n '1,/^wait mods-done/p' "$T/falcon-tos-keys.txt"
		for i in 1 2 3; do
			printf '%s\n' 'type T=/tests; [ -x $T/starttos ] || T=/usr/bin; C=$T/tosml.img; [ -f $C ] || C=/etc/tos/tosml.img; $T/starttos -C /tmp/c -c $C 2> /tmp/st.log\n'
			echo "shot $OUT/tosint-$i.png desktop"
			printf 'event keydown 0x1d\nevent keydown 0x38\nevent keypress 0x01\nevent keyup 0x38\nevent keyup 0x1d\nsleep 5\n'
			printf 'event keydown 0x1d\nevent keypress 0x2e\nevent keyup 0x1d\nwait ^C\n'
			# a process that outlives the session, as a daemon would
			printf 'type sleep 1200 & cat /tmp/st.log; echo int-%s-done\\n\nwait int-%s-done\n' $i $i
		done
		echo "debug quit 0"; } > "$OUT/tosint-keys.txt"
		run tosint vga 1200
		check "$OUT/tosint.log.out" 'int-1-done' 'int-2-done' 'int-3-done'
		! grep -q 'contiguous\|PANIC\|panic' "$OUT/tosint.log.out" || { echo "[FAIL] tosint: no ST-RAM or kernel error"; fail=1; }
		for i in 1 2 3; do python3 "$T/tosshot.py" desktop "$OUT/tosint-$i.png" || fail=1; done ;;
	tosloop)
		n=${BOOTS:-15}
		{ sed -n '1,/^wait mods-done/p' "$T/falcon-tos-keys.txt"
		i=1; while [ $i -le $n ]; do
			printf '%s\n' 'type T=/tests; [ -x $T/starttos ] || T=/usr/bin; C=$T/tosml.img; [ -f $C ] || C=/etc/tos/tosml.img; $T/starttos -C /tmp/c -c $C -v 2> /tmp/st.log\n'
			echo "shot $OUT/tosloop-$i.png desktop"
			# the hot key to the console, then Ctrl-C there ends the session
			printf 'event keydown 0x1d\nevent keydown 0x38\nevent keypress 0x01\nevent keyup 0x38\nevent keyup 0x1d\nsleep 5\n'
			printf 'event keydown 0x1d\nevent keypress 0x2e\nevent keyup 0x1d\nwait ^C\n'
			printf 'type cat /tmp/st.log; echo boot-%s-done\\n\nwait boot-%s-done\n' $i $i
			i=$((i + 1))
		done
		echo "debug quit 0"; } > "$OUT/tosloop-keys.txt"
		run tosloop vga $((n * 200 + 600))
		i=1; while [ $i -le $n ]; do
			python3 "$T/tosshot.py" desktop "$OUT/tosloop-$i.png" || fail=1
			i=$((i + 1))
		done ;;
	cubase)
		D=guest/TOS/CUBASE/CUBASE31
		python3 "$T/homefile.py" "$IMG" $D || { echo "[SKIP] cubase: no ~$D on the image"; continue; }
		n=${CUBOOTS:-5}
		{ printf '%s\n' 'wait login:' 'type root\n' 'wait # '
		i=1; while [ $i -le $n ]; do
			# 10-second counters while the guest runs
			printf '%s\n' 'type (until /tests/tosstat > /dev/null 2>&1; do sleep 5; done; while /tests/tosstat 10; do :; done) > /tmp/cs.log 2>&1 &\n'
			printf "%s\n" "type /usr/bin/starttos -M -C /home/$D -v 2> /tmp/st.log\\n"
			echo "shot $OUT/cubase-$i.png arrange"
			# then the hot key to the console, whose Ctrl-C ends the session
			printf 'sleep 30\nevent keydown 0x1d\nevent keydown 0x38\nevent keypress 0x01\nevent keyup 0x38\nevent keyup 0x1d\nsleep 5\n'
			printf 'event keydown 0x1d\nevent keypress 0x2e\nevent keyup 0x1d\nwait ^C\nsleep 5\n'
			printf "%s\n" "type sleep 12; sed 's/^/cs-$i: /' /tmp/cs.log; echo cub-$i-done\\n"
			echo "wait cub-$i-done"
			i=$((i + 1))
		done
		echo "debug quit 0"; } > "$OUT/cubase-keys.txt"
		w=$WAIT; WAIT=${CUBWAIT:-900}
		# about 50000 VBLs a start
		FRAMES=$((n * 60000 + 100000))
		run cubase vga $((n * WAIT + 600))
		WAIT=$w FRAMES=
		i=1; while [ $i -le $n ]; do
			python3 "$T/tosshot.py" arrange "$OUT/cubase-$i.png" || fail=1
			# 100 s at the Arrange window, before the one holding the end: no hardware access
			# (movep is Cubase's own)
			l=$(grep -a "^cs-$i: tosstat: absent" "$OUT/cubase.log.out" | tail -11 | head -10)
			k=$(printf '%s\n' "$l" | grep -c 'absent 0 top 0 berr 0 buserr 0 iow 0 mmu 0 illegal 0[^0-9]*$')
			if [ "$k" -eq 10 ]; then echo "[OK] cubase $i: steady"
			else echo "[FAIL] cubase $i: $k of 10 samples steady"; fail=1; fi
			i=$((i + 1))
		done ;;
	esac
done
[ $fail -eq 0 ] && echo "[OK] falcon check" || { echo "[FAIL] falcon check"; exit 1; }
