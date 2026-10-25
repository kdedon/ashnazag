#!/bin/sh
# build.sh -- Vim's help files for the vim package, without the release
# notes and the files whose names exceed 14 bytes.
#
#   sh images/ports/vimdoc/build.sh outdir
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=vimdoc OUT=$1
. "$AUX/images/ports/port.sh"
S=$W/$(src vim)
H=$ROOT/opt/amix/share/vim/vim92/doc
mkdir -p "$H"
cp "$S"/runtime/doc/*.txt "$S/runtime/doc/tags" "$H/"
rm -f "$H"/version*.txt
find "$H" -name '???????????????*' -exec rm {} +
portpkg
