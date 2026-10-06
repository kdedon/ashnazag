#!/bin/sh
set -eu
umask 077

D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
if [ "$#" -lt 1 ] || [ "$#" -gt 4 ]; then
	echo "usage: mkamiga.sh new-output-directory [AmigaOS-CD.iso [module-directory [Picasso96.lha]]]" >&2
	exit 2
fi
OUT=$1
ISO=${2:-$AUX/AmigaOS3.2CD.iso}
MODULES=${3:-}
PICASSO=${4:-$AUX/Picasso96.lha}
# MUI, AmiSSL and IBrowse, installed as their installers would
AMIGAAPPS=${AMIGAAPPS-$AUX/media/amiga}
[ "$#" -lt 4 ] || [ -f "$PICASSO" ] || { echo "missing Picasso96 archive: $PICASSO" >&2; exit 1; }
[ -f "$ISO" ] || { echo "missing CD: $ISO" >&2; exit 1; }
if [ -n "$MODULES" ]; then
	for m in guestcore amigaguest; do
		[ -f "$MODULES/$m" ] || { echo "missing module: $MODULES/$m" >&2; exit 1; }
	done
fi
mkdir -- "$OUT"
OUT=$(cd "$OUT" && pwd)
nice -n 19 python3 "$AUX/tools/amiga/media.py" extract "$ISO" "$OUT/media" > "$OUT/extraction.json"
sh "$AUX/kernel/guest/amiga/build.sh" "$OUT/bin"
sh "$AUX/kernel/guest/amiga/rtg/build.sh" "$OUT/rtg"
cp "$AUX/kernel/guest/amiga/rtg/README.md" "$AUX/kernel/guest/amiga/rtg/NOTICE" "$OUT/rtg/"

TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
O=$OUT/registration
mkdir "$O"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -D__STDC__=0 \
	-I"$AUX/kernel/dlm/include" -I"$AUX/kernel/guest/include" \
	-c "$D/amigareg.c" -o "$O/amigareg.o"
for s in modpath modadm; do
	"$TC/bin/m68k-cbm-sysv4-as" -o "$O/$s.o" "$AUX/kernel/dlm/libmod/$s.s"
done
"$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/bin/amigareg" \
	"$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" "$O/amigareg.o" \
	"$O/modpath.o" "$O/modadm.o" "$SYS/usr/lib/libc.so.1" "$SYS/usr/ccs/lib/crtn.o"

ROOT=$OUT/root
cp "$D/installmig" "$OUT/"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/sbin" "$ROOT/etc/amiga" "$ROOT/amiga/media"
cp "$OUT/bin/startmig" "$ROOT/usr/bin/"
cp "$D/makeamiga" "$ROOT/usr/bin/"
cp "$OUT/bin/amigareg" "$ROOT/usr/sbin/"
cp "$OUT/media/ROM/kicka4000.rom" "$ROOT/etc/amiga/"
cp "$OUT/media/ADF/"*.adf "$ROOT/amiga/media/"
cp "$OUT/media/manifest.json" "$ROOT/amiga/media/"
mkdir -p "$ROOT/amiga/rtg"
cp "$OUT/rtg/container.card" "$OUT/rtg/README.md" "$OUT/rtg/NOTICE" "$ROOT/amiga/rtg/"
sh "$AUX/kernel/guest/amiga/dos/build.sh" "$ROOT/amiga/guest/dos"
sh "$AUX/kernel/guest/amiga/input/build.sh" "$ROOT/amiga/guest/input"
sh "$AUX/kernel/guest/amiga/session/build.sh" "$ROOT/amiga/guest/session"
cp "$AUX/kernel/guest/amiga/dos/README.md" "$ROOT/amiga/guest/dos/"
cp "$AUX/kernel/guest/amiga/dos/CONTAINER" "$ROOT/amiga/guest/dos/"
cp "$AUX/kernel/guest/amiga/input/README.md" "$ROOT/amiga/guest/input/"
cp "$ROOT/amiga/guest/dos/container-boot.rom" "$ROOT/etc/amiga/"
nice -n 19 python3 "$AUX/tools/amiga/template.py" "$OUT/media/ADF" "$ROOT/amiga/sys" \
	--input "$ROOT/amiga/guest/input/container-input" --card "$ROOT/amiga/rtg/container.card" \
	--session "$ROOT/amiga/guest/session/Session"
if [ -f "$PICASSO" ]; then
	cp "$PICASSO" "$ROOT/amiga/rtg/Picasso96.lha"
	(cd "$ROOT/amiga/rtg" && sha256sum Picasso96.lha > Picasso96.sha256)
	# the Picasso96 runtime the boot extension binds container.card with
	7z x -y -o"$OUT/p96" "$PICASSO" > /dev/null
	P=$OUT/p96/Picasso96Install
	for f in Picasso96/rtg.library Picasso96/emulation.library Picasso96/fastlayers.library Picasso96API.library; do
		cp "$P/Libs/$f" "$ROOT/amiga/sys/LIBS/$f"
	done
	cp "$P/Devs/Monitors/Picasso96" "$ROOT/amiga/sys/DEVS/Monitors/Container"
	rm -rf "$OUT/p96"
fi
if [ -n "$AMIGAAPPS" ] && [ -f "$AMIGAAPPS/SHA256SUMS" ]; then
	nice -n 19 python3 "$AUX/tools/amiga/apps.py" "$AMIGAAPPS" "$ROOT/amiga/sys"
fi
if [ -n "$MODULES" ]; then
	mkdir -p "$ROOT/usr/lib/amiga/mod.d"
	cp "$MODULES/guestcore" "$MODULES/amigaguest" "$ROOT/usr/lib/amiga/mod.d/"
	if [ -f "$MODULES/../kernel.sha256" ]; then
		cp "$MODULES/../kernel.sha256" "$OUT/kernel.sha256"
	fi
fi
printf '%s\n' 'Local staging complete; see images/amigaenv/README.md for installation and limits.'
