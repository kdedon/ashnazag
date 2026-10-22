#!/bin/sh
# makelisa.sh ENV [IMAGE]: install the files of the user's Lisa ProFile image (DC42 or raw 532-byte
# blocks) into ~/Lisa/ENV, one host file each; office, the default environment, is ~/Lisa itself.
# The image is only read.
set -e
AUX=$(cd "$(dirname "$0")/.." && pwd)
ENV=${1:?usage: makelisa.sh ENV [IMAGE]}
IMAGE=${2:-$AUX/media/lisa/imported/profile.dc42}
[ -f "$IMAGE" ] || { echo "makelisa: no ProFile image $IMAGE" >&2; exit 1; }
case $ENV in
office) DEST=$HOME/Lisa ;;
*) DEST=$HOME/Lisa/$ENV ;;
esac
nice -n 19 python3 -I "$AUX/tools/lisavol.py" import "$IMAGE" "$DEST" | sed "s#^#makelisa: $DEST: #"
