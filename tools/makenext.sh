#!/bin/sh
# makenext.sh ENV [ISO]: install the user's OPENSTEP CD tree into ~/NeXT/ENV, as its installer would.
# Owners and modes go to ~/NeXT/ENV.meta; device nodes become NAME.__special__ files.
set -e
AUX=$(cd "$(dirname "$0")/.." && pwd)
ENV=${1:?usage: makenext.sh ENV [ISO]}
ISO=${2:-$AUX/media/openstep/Openstep-4.2-Intel-User.iso}
[ -f "$ISO" ] || { echo "makenext: no CD image $ISO" >&2; exit 1; }
DEST=$HOME/NeXT/$ENV
mkdir -p "$HOME/NeXT"
nice -n 19 python3 -I "$AUX/tools/nextcd.py" -m "$DEST.meta.new" "$ISO" extract "$DEST" /
# what the installer and BuildDisk leave: no CD marker, an fstab without the disk (the kernel mounts the
# folder), a 16 MB swap file, the install language, the marker of a finished install (without it
# loginwindow runs the installer's second stage)
rm -rf "$DEST/NextCD"
grep -v ' \./NextCD\(/\|$\)' "$DEST.meta.new" > "$DEST.meta"
rm "$DEST.meta.new"
sed -i '/^\/dev\//d' "$DEST/private/etc/fstab"
echo LANGUAGE=English > "$DEST/private/adm/CDIS.custom"
echo "100644 0 0 $(date +%s) ./private/adm/CDIS.custom" >> "$DEST.meta"
: > "$DEST/private/adm/BuildDisk.custom"
echo "100644 0 0 $(date +%s) ./private/adm/BuildDisk.custom" >> "$DEST.meta"
# links to the root or to one of their own folders (the netboot server's tftpboot) would trap host
# programs that walk the folder through links
(cd "$DEST" && find . -type l -print) | while read -r l; do
	t=$(readlink "$DEST/$l")
	if [ "$t" = / ] || expr "$t" : '\(\.\.*/*\)*$' > /dev/null; then
		rm "$DEST/$l"
		echo "$l"
	fi
done > "$DEST.loops"
awk 'NR == FNR { d[$0]; next } { p = $0; sub(/^[^ ]* [^ ]* [^ ]* [^ ]* /, "", p) } !(p in d)' \
	"$DEST.loops" "$DEST.meta" > "$DEST.meta.new"
mv "$DEST.meta.new" "$DEST.meta"
rm "$DEST.loops"
rm -f "$DEST/private/vm/swapfile"
truncate -s 16M "$DEST/private/vm/swapfile"
echo "101600 0 0 $(date +%s) ./private/vm/swapfile" >> "$DEST.meta"
echo "makenext: $DEST ($(wc -l < "$DEST.meta") entries)"
