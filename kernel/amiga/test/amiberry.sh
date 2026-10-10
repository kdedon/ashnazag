#!/bin/sh
# amiberry.sh -- boot the mkhdf.sh disk in Amiberry, headless: an A3000
# with a 68040, a Prometheus and PCI cards.  Serial goes to TCP 1234.
#
#   sh amiberry.sh hdfdir [seconds]
#
# AMIBERRY: the install prefix (default toolchain/amiberry/usr).
# KICK: an A3000 Kickstart ROM.  STATE: config, home and IPC socket
# directory (default hdfdir/run).  CARDS: Amiberry board names on the
# PCI bus (default "ne2000_pci es1370 fm801"); GFX: a PCI graphics card
# type (default S3VIRGE_PCI, empty for none).  The NE2000 is on the
# user-mode network; host port ECHOPORT (default 2307) reaches the
# guest's TCP echo service.  Runs until killed, or for
# the given seconds.
set -e
A=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$A/../.." && pwd)
H=$(cd "$1" && pwd)
AB=${AMIBERRY:-$AUX/toolchain/amiberry/usr}
ST=${STATE:-$H/run}
CARDS=${CARDS-ne2000_pci es1370 fm801}
GFX=${GFX-S3VIRGE_PCI}
[ -f "$KICK" ] || { echo "[FAIL] KICK: no Kickstart ROM"; exit 1; }
mkdir -p "$ST/home/.config" "$ST/xdg"
chmod 700 "$ST/xdg"
C=$ST/amix.uae
cat > "$C" <<EOF
config_hardware=true
use_gui=no
kickstart_rom_file=$KICK
chipset=ecs
chipset_compatible=A3000
cpu_model=68040
fpu_model=68040
mmu_model=68040
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
filesystem2=rw,DH0:AmigaBoot:$H/boot,5
hardfile2=rw,SD6:$H/amix.hdf,0,0,0,512,-128,,scsi6_a3000
serial_port=TCP://127.0.0.1:1234
serial_direct=true
sound_output=none
prometheus_rom_file=:ENABLED
EOF
for c in $CARDS; do
	echo "${c}_rom_file=:ENABLED" >> "$C"
done
case " $CARDS " in *" ne2000_pci "*)
	printf 'ne2000_pci=slirp\nslirp_redir=tcp:%s:7:10.0.2.15\n' "${ECHOPORT:-2307}" >> "$C" ;;
esac
if [ -n "$GFX" ]; then
	printf 'gfxcard_type=%s\ngfxcard_size=4\n' "$GFX" >> "$C"
fi
export HOME=$ST/home XDG_CONFIG_HOME=$ST/home/.config XDG_RUNTIME_DIR=$ST/xdg
export LD_LIBRARY_PATH=$AB/lib/x86_64-linux-gnu:$AB/lib/x86_64-linux-gnu/amiberry
export SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
if [ -n "$2" ]; then
	exec timeout "$2" nice -n 19 "$AB/bin/amiberry" -f "$C" -G
fi
exec nice -n 19 "$AB/bin/amiberry" -f "$C" -G
