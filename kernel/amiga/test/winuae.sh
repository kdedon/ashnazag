#!/bin/sh
# winuae.sh -- boot the mkhdf.sh disk in WinUAE under Wine on a virtual
# X display: an A3000 with a 68060, a Prometheus, a Voodoo3 3000 and an
# RTL8029 on PCI.  Serial goes to TCP port SERPORT (default 1235).
#
#   sh winuae.sh hdfdir [seconds]
#
# WINUAE: the directory setup.sh --winuae fills (default
# toolchain/winuae): Wine prefix, emulator, state.  KICK: an A3000
# Kickstart ROM.  CPU: 68060 (default) or 68040.  GFX: the graphics
# card (default V3_3000; empty for none).  VOODOO3: the card's BIOS,
# copied in as voodoo3.rom.  CARDS: WinUAE board names on the PCI bus
# (default ne2000_pci; host port ECHOPORT, default 2308, reaches the
# guest's echo service).  XDISPLAY (default :77): the virtual display,
# started when not running and left up.  The console login is on the screen:
#   python3 xtype.py :77 root      types a line
#   ffmpeg -f x11grab -video_size 740x630 -i :77 -frames:v 1 s.png
# Runs until killed, or for the given seconds.
set -e
A=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$A/../.." && pwd)
H=$(cd "$1" && pwd)
W=${WINUAE:-$AUX/toolchain/winuae}
CPU=${CPU:-68060}
CARDS=${CARDS-ne2000_pci}
XD=${XDISPLAY:-:77}
GFX=${GFX-V3_3000}
[ -f "$KICK" ] || { echo "[FAIL] KICK: no Kickstart ROM"; exit 1; }
[ -f "$W/bin/winuae64.exe" ] || { echo "[FAIL] no WinUAE; run tools/setup.sh --winuae"; exit 1; }
mkdir -p "$W/run"
exec 9> "$W/run/lock"
flock -n 9 || { echo "[FAIL] WinUAE already running"; exit 1; }
XVFB=$(command -v Xvfb || echo "$W/xvfb/usr/bin/Xvfb")
export WINEPREFIX=$W/prefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" DISPLAY=$XD
if ! xdpyinfo > /dev/null 2>&1; then
	nice -n 19 "$XVFB" "$XD" -screen 0 1280x1024x24 -nolisten tcp > "$W/xvfb.log" 2>&1 9>&- &
	echo $! > "$W/xvfb.pid"
	n=0
	until xdpyinfo > /dev/null 2>&1; do
		n=$((n + 1)); [ $n -lt 20 ] || { echo "[FAIL] no X display $XD"; exit 1; }
		sleep 1
	done
fi
[ -f "$WINEPREFIX/system.reg" ] || nice -n 19 wine wineboot -i > "$W/wineboot.log" 2>&1
# Wine maps Z: to /
win() { echo "Z:$1" | tr / '\\'; }
rm -f "$W/bin/voodoo3.rom"
if [ -n "$VOODOO3" ]; then
	[ -f "$VOODOO3" ] || { echo "[FAIL] VOODOO3: $VOODOO3 not found"; exit 1; }
	cp "$VOODOO3" "$W/bin/voodoo3.rom"
fi
C=$W/run/amix.uae
cat > "$C" <<EOF
config_hardware=true
use_gui=no
kickstart_rom_file=$(win "$KICK")
chipset=ecs
chipset_compatible=A3000
cpu_model=$CPU
fpu_model=$CPU
mmu_model=$CPU
cpu_compatible=false
cpu_24bit_addressing=false
cpu_speed=max
cachesize=0
chipmem_size=4
a3000mem_size=16
mbresmem_size=32
fastmem_size=0
z3mem_size=0
z3mapping=uae
filesystem2=rw,DH0:AmigaBoot:$(win "$H/boot"),5
hardfile2=rw,SD6:$(win "$H/amix.hdf"),0,0,0,512,-128,,scsi6_a3000
serial_port=TCP://127.0.0.1:${SERPORT:-1235}
serial_direct=true
sound_output=none
gfx_api=directdraw
prometheus_rom_file=:ENABLED
EOF
for c in $CARDS; do
	echo "${c}_rom_file=:ENABLED" >> "$C"
done
case " $CARDS " in *" ne2000_pci "*)
	printf 'ne2000_pci=slirp\nslirp_redir=tcp:%s:7:10.0.2.15\n' "${ECHOPORT:-2308}" >> "$C" ;;
esac
[ -z "$GFX" ] || printf 'gfxcard_type=%s\ngfxcard_size=16\n' "$GFX" >> "$C"
cd "$W/bin"
set -- ${2:+timeout "$2"} nice -n 19 wine winuae64.exe -portable -log -f "$(win "$C")"
exec "$@"
