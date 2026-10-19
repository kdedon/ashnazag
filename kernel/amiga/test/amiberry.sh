#!/bin/sh
# amiberry.sh -- boot the mkhdf.sh disk in Amiberry, headless: an A3000
# with a 68040, a Prometheus and PCI cards.  Serial goes to TCP port
# SERPORT (default 1234).
#
#   sh amiberry.sh hdfdir [seconds]
#
# AMIBERRY: the install prefix (default toolchain/amiberry-local/usr, from
# toolchain/build-amiberry.sh, else toolchain/amiberry/usr).
# KICK: a Kickstart ROM for the machine.  CPU: 68040 (default) or 68060.
# MACHINE: a3000 (default; the disk is SCSI unit 6) or a4000 (AGA; the
# disk is the IDE master).  CSPPC: the CyberStorm PPC's flash image; the
# card replaces the CPU and its RAM, the disk is unit CSPPCID (default 0)
# on its SCSI; CSPPCMB its RAM (default 128).  MBMEM: motherboard fast
# RAM in MB (default 16).  CDROM: an ISO image as a CD-ROM on that SCSI, unit 1.
# STATE: config, home and IPC socket
# directory (default hdfdir/run).  CARDS: Amiberry board names on the
# PCI bus (default "ne2000_pci es1370 fm801").  GFX: a graphics card
# (default ZZ9000_Z2; S3VIRGE_PCI or VOODOO3 puts one on PCI; empty for
# none).  VOODOO3: the Voodoo3's BIOS (default media/amiga/voodoo3.rom).
# MEDWIN: 256 or 512 puts a Mediator 4000 Mk II with that window in place
# of the Prometheus.  PCIDMA=1 lets the bridge's cards master into
# Amiga RAM (real Mediators and Prometheus cannot).  Z3MAP: Zorro III placement (default uae; real
# starts at 0x40000000).
# The NE2000 is on the user-mode network; host port ECHOPORT (default
# 2307) reaches the guest's TCP echo service.  DF0-DF3: ADF files in
# the floppy drives.  UAEOPTS: extra config lines, last wins (e.g.
# cpu_data_cache=true for a real 060's caches).  Runs until killed, or
# for the given seconds.
set -e
A=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$A/../.." && pwd)
H=$(cd "$1" && pwd)
AB=$AUX/toolchain/amiberry-local/usr
[ -x "$AB/bin/amiberry" ] || AB=$AUX/toolchain/amiberry/usr
AB=${AMIBERRY:-$AB}
ST=${STATE:-$H/run}
CARDS=${CARDS-ne2000_pci es1370 fm801}
GFX=${GFX-ZZ9000_Z2}
CPU=${CPU:-68040}
MACHINE=${MACHINE:-a3000}
[ -f "$KICK" ] || { echo "[FAIL] KICK: no Kickstart ROM"; exit 1; }
mkdir -p "$ST/home/.config" "$ST/xdg"
chmod 700 "$ST/xdg"
C=$ST/amix.uae
cat > "$C" <<EOF
config_hardware=true
use_gui=no
kickstart_rom_file=$KICK
$(if [ "$MACHINE" = a4000 ]; then
	echo chipset=aga; echo chipset_compatible=A4000
else
	echo chipset=ecs; echo chipset_compatible=A3000
fi)
cpu_model=$CPU
fpu_model=$CPU
mmu_model=$CPU
cpu_compatible=false
cpu_24bit_addressing=false
cpu_speed=max
cachesize=0
chipmem_size=4
a3000mem_size=${MBMEM:-16}
$(if [ -n "$CSPPC" ]; then
	echo cpuboard_type=CyberStormPPC; echo "cpuboard_rom_file=$CSPPC"; echo cpuboardmem1_size=${CSPPCMB:-128}
else
	echo mbresmem_size=32
fi)
fastmem_size=0
z3mem_size=0
z3mapping=${Z3MAP:-uae}
$([ ! -d "$H/boot" ] || echo "filesystem2=rw,DH0:AmigaBoot:$H/boot,5")
$(if [ -n "$CSPPC" ]; then
	echo "hardfile2=rw,SD${CSPPCID:-0}:$H/amix.hdf,0,0,0,512,-128,,scsi${CSPPCID:-0}_cpuboard"
	[ -z "$CDROM" ] || printf 'cdimage0=%s,image\nuaehf2=cd0,ro,:,0,0,0,2048,0,,scsi1_cpuboard\n' "$CDROM"
elif [ "$MACHINE" = a4000 ]; then
	echo "hardfile2=rw,DH1:$H/amix.hdf,0,0,0,512,0,,ide0_mainboard"
else
	echo "hardfile2=rw,SD6:$H/amix.hdf,0,0,0,512,-128,,scsi6_a3000"
fi)
serial_port=TCP://127.0.0.1:${SERPORT:-1234}
serial_direct=true
sound_output=none
$(if [ -n "$MEDWIN" ]; then
	echo mediator_rom_file=:ENABLED
	echo "mediator_rom_options=subtype=4000mkii$([ "$MEDWIN" = 256 ] || echo ,winsize)${PCIDMA:+,fulldma}"
else
	echo prometheus_rom_file=:ENABLED
	[ -z "$PCIDMA" ] || echo prometheus_rom_options=fulldma
fi)
EOF
for c in $CARDS; do
	echo "${c}_rom_file=:ENABLED" >> "$C"
done
case " $CARDS " in *" ne2000_pci "*)
	printf 'ne2000_pci=slirp\nslirp_redir=tcp:%s:7:10.0.2.15\n' "${ECHOPORT:-2307}" >> "$C" ;;
esac
n=0
for d in "${DF0-}" "${DF1-}" "${DF2-}" "${DF3-}"; do
	if [ -n "$d" ]; then
		printf 'floppy%d=%s\nfloppy%dtype=0\n' $n "$d" $n >> "$C"
		last=$n
	fi
	n=$((n + 1))
done
[ -z "${last-}" ] || echo "nr_floppies=$((last + 1))" >> "$C"
[ -z "$UAEOPTS" ] || printf '%s\n' "$UAEOPTS" | tr -s ' ' '\n' >> "$C"
if [ -n "$GFX" ]; then
	# Amiberry calls the Voodoo3 V3_3000; an unknown name gives its own Zorro II card
	if [ "$GFX" = VOODOO3 ]; then
		printf 'gfxcard_type=V3_3000\ngfxcard_size=16\n' >> "$C"
	else
		printf 'gfxcard_type=%s\ngfxcard_size=4\n' "$GFX" >> "$C"
	fi
fi
# the PCI cards' BIOS comes from the ROM path
mkdir -p "$ST/roms"
rm -f "$ST/roms/voodoo3.rom"
if [ "$GFX" = VOODOO3 ]; then
	V=${VOODOO3:-$AUX/media/amiga/voodoo3.rom}
	[ -f "$V" ] || { echo "[FAIL] VOODOO3: $V not found"; exit 1; }
	cp "$V" "$ST/roms/voodoo3.rom"
fi
export HOME=$ST/home XDG_CONFIG_HOME=$ST/home/.config XDG_RUNTIME_DIR=$ST/xdg
export LD_LIBRARY_PATH=$AB/lib/x86_64-linux-gnu:$AB/lib/x86_64-linux-gnu/amiberry
export SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
if [ -n "$2" ]; then
	exec timeout "$2" nice -n 19 "$AB/bin/amiberry" -o rom_path="$ST/roms/" -f "$C" -G
fi
exec nice -n 19 "$AB/bin/amiberry" -o rom_path="$ST/roms/" -f "$C" -G
