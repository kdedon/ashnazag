#!/bin/sh
# build.sh -- bzip2 1.0.8 for /opt/amix.
#
#   sh images/ports/bzip2/build.sh outdir
#
# The patch prints -v statistics in integers (no FPU needed) and gives
# bzip2recover 32-bit offsets (files up to 512 MB).
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=bzip2 OUT=$1
. "$AUX/images/ports/port.sh"
S=$W/$(src bzip2)
(cd "$S" && patch -p1 -s < "$D/bzip2.diff" &&
	nice -n 19 make -j1 CC="$CC" CFLAGS="$PCFLAGS -Wall" BIGFILES= AR="$AUX/toolchain/bin/m68k-elf-ar" \
		RANLIB=: bzip2 bzip2recover) > "$W/build.log" 2>&1 || { tail -20 "$W/build.log"; exit 1; }
B=$ROOT/opt/amix
mkdir -p "$B/man/man1" "$B/doc/bzip2"
cp "$S/bzip2" "$S/bzip2recover" "$B/bin/"
printf '#!/bin/sh\nexec /opt/amix/bin/bzip2 -d "$@"\n' > "$B/bin/bunzip2"
printf '#!/bin/sh\nexec /opt/amix/bin/bzip2 -dc "$@"\n' > "$B/bin/bzcat"
chmod 755 "$B/bin/bunzip2" "$B/bin/bzcat"
cp "$S/bzip2.1" "$B/man/man1/"
cp "$S/LICENSE" "$S/README" "$B/doc/bzip2/"
portpkg
