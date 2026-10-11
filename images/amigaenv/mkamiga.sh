#!/bin/sh
set -eu
umask 077

D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
	echo "usage: mkamiga.sh new-output-directory [AmigaOS-CD.iso [Picasso96.lha|Picasso96-directory]]" >&2
	exit 2
fi
OUT=$1
ISO=${2:-$AUX/AmigaOS3.2CD.iso}
PICASSO=${3:-$AUX/Picasso96.lha}
# MUI, AmiSSL and IBrowse, installed as their installers would
AMIGAAPPS=${AMIGAAPPS-$AUX/media/amiga}
[ "$#" -lt 3 ] || [ -e "$PICASSO" ] || { echo "missing Picasso96 archive: $PICASSO" >&2; exit 1; }
[ -f "$ISO" ] || { echo "missing CD: $ISO" >&2; exit 1; }
mkdir -- "$OUT"
OUT=$(cd "$OUT" && pwd)
nice -n 19 python3 "$AUX/tools/amiga/media.py" extract "$ISO" "$OUT/media" > "$OUT/extraction.json"
sh "$AUX/kernel/guest/amiga/rtg/build.sh" "$OUT/rtg"
cp "$AUX/kernel/guest/amiga/rtg/README.md" "$AUX/kernel/guest/amiga/rtg/NOTICE" "$OUT/rtg/"

ROOT=$OUT/root
mkdir -p "$ROOT/etc/amiga" "$ROOT/amiga/media"
cp "$OUT/media/ROM/kicka4000.rom" "$OUT/media/ROM/kicka3000.rom" "$ROOT/etc/amiga/"
cp "$OUT/media/ADF/"*.adf "$ROOT/amiga/media/"
cp "$OUT/media/manifest.json" "$ROOT/amiga/media/"
mkdir -p "$ROOT/amiga/rtg"
cp "$OUT/rtg/container.card" "$OUT/rtg/README.md" "$OUT/rtg/NOTICE" "$ROOT/amiga/rtg/"
sh "$AUX/kernel/guest/amiga/dos/build.sh" "$ROOT/amiga/guest/dos"
sh "$AUX/kernel/guest/amiga/input/build.sh" "$ROOT/amiga/guest/input"
sh "$AUX/kernel/guest/amiga/session/build.sh" "$ROOT/amiga/guest/session"
sh "$AUX/kernel/guest/amiga/bsdsock/build.sh" "$ROOT/amiga/guest/bsdsock"
sh "$AUX/kernel/guest/amiga/ahi/build.sh" "$ROOT/amiga/guest/ahi"
cp "$AUX/kernel/guest/amiga/dos/README.md" "$ROOT/amiga/guest/dos/"
cp "$AUX/kernel/guest/amiga/dos/CONTAINER" "$ROOT/amiga/guest/dos/"
cp "$AUX/kernel/guest/amiga/input/README.md" "$ROOT/amiga/guest/input/"
cp "$ROOT/amiga/guest/dos/container-boot.rom" "$ROOT/etc/amiga/"
nice -n 19 python3 "$AUX/tools/amiga/template.py" "$OUT/media/ADF" "$ROOT/amiga/sys" \
	--input "$ROOT/amiga/guest/input/container-input" --card "$ROOT/amiga/rtg/container.card" \
	--session "$ROOT/amiga/guest/session/Session" \
	--bsdsocket "$ROOT/amiga/guest/bsdsock/bsdsocket.library" \
	--audio "$ROOT/amiga/guest/ahi/container.audio"
if [ -f "$PICASSO" ] || [ -d "$PICASSO/Picasso96Install" ]; then
	# the Picasso96 runtime the boot extension binds container.card with
	if [ -d "$PICASSO" ]; then
		P=$PICASSO/Picasso96Install
	else
		cp "$PICASSO" "$ROOT/amiga/rtg/Picasso96.lha"
		(cd "$ROOT/amiga/rtg" && sha256sum Picasso96.lha > Picasso96.sha256)
		7z x -y -o"$OUT/p96" "$PICASSO" > /dev/null
		P=$OUT/p96/Picasso96Install
	fi
	for f in Picasso96/rtg.library Picasso96/emulation.library Picasso96/fastlayers.library Picasso96API.library; do
		cp "$P/Libs/$f" "$ROOT/amiga/sys/LIBS/$f"
	done
	cp "$P/Devs/Monitors/Picasso96" "$ROOT/amiga/sys/DEVS/Monitors/Container"
	rm -rf "$OUT/p96"
fi
if [ -n "$AMIGAAPPS" ] && [ -f "$AMIGAAPPS/SHA256SUMS" ]; then
	nice -n 19 python3 "$AUX/tools/amiga/apps.py" "$AMIGAAPPS" "$ROOT/amiga/sys"
fi
printf '%s\n' 'Local staging complete; see images/amigaenv/README.md for installation and limits.'
