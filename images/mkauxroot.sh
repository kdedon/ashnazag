#!/bin/sh
# Extract the A/UX 3.1 root tree from the A/UX 3.1 CD's root partition.
#
#	sh images/mkauxroot.sh [cd.iso [dest]]
#
# Defaults: media/aux-3.1.iso (tools/setup.sh --aux-cd), images/work/auxroot-cd.
# Device nodes become NAME.__special__ files.  dest.meta lists every entry's
# mode, uid, gid and mtime, since extraction as a user keeps no owners.
set -eu

AUX=$(cd "$(dirname "$0")/.." && pwd)
ISO=${1:-$AUX/media/aux-3.1.iso}
DEST=${2:-$AUX/images/work/auxroot-cd}
PART="UNIX Root&Usr slice 0"

die() { echo "mkauxroot: $*" >&2; exit 1; }
[ -f "$ISO" ] || die "missing $ISO (tools/setup.sh --aux-cd)"

rm -rf "$DEST.new"
nice -n 19 python3 "$AUX/tools/ufs.py" -p "$PART" -m "$DEST.meta.new" \
    "$ISO" "$DEST.new" 2> "$DEST.err" || die "extraction failed"
[ ! -s "$DEST.err" ] || { cat "$DEST.err" >&2; die "extraction errors"; }
rm -f "$DEST.err"
[ -f "$DEST.new/mac/bin/startmac" ] || die "$ISO: no A/UX root in $PART"
rm -rf "$DEST"
mv "$DEST.new" "$DEST"
mv "$DEST.meta.new" "$DEST.meta"
echo "mkauxroot: $DEST ($(wc -l < "$DEST.meta") entries)"
