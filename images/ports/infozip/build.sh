#!/bin/sh
# build.sh -- Info-ZIP zip 3.0 and unzip 6.0 for /opt/amix.
#
#   sh images/ports/infozip/build.sh outdir
#
# The flags configure would find are given directly: its run tests
# cannot cross-compile.  No large files, no bzip2 method, no Unicode
# names (libc has no wchar.h).  unzip gets Debian's CVE fixes
# (2014-8139..8141, 2014-9636, 2014-9913, 2015-7696/7697, 2016-9844,
# 2018-1000035, 2019-13232, 2022-0529/0530) and two overflow fixes.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=infozip OUT=$1
. "$AUX/images/ports/port.sh"
Z=$W/$(src zip)
U=$W/$(src unzip)
tar xJf "$(sh "$AUX/images/fetch.sh" unzip-debian)" -C "$W" debian/patches
for n in 06 07 09 10 11 12 14 15 16 18 19 20 22 23 24 25 26 28; do
	patch -d "$U" -p1 -s < "$(ls "$W"/debian/patches/$n-*)"
done
(cd "$Z" && nice -n 19 make -j1 -f unix/Makefile zips CC="$CC" CPP="$CC -E" IZ_BZIP2= \
	CFLAGS="-I. -DUNIX -DNO_LARGE_FILE_SUPPORT -DUIDGID_NOT_16BIT -DHAVE_DIRENT_H -DHAVE_TERMIOS_H $PCFLAGS" \
	OBJA= OCRCU8=crc32_.o OCRCTB= LFLAGS1= LFLAGS2= &&
 cd "$U" && nice -n 19 make -j1 -f unix/Makefile unzips CC="$CC" LD="$CC" CFLAGS="$PCFLAGS" \
	LOCAL_UNZIP="-DNO_LARGE_FILE_SUPPORT -DNO_LCHMOD" IZ_BZIP2=. LF2= HOST_VERSINFO=) \
	> "$W/build.log" 2>&1 || { tail -20 "$W/build.log"; exit 1; }
! grep 'implicit declaration' "$W/build.log" || exit 1
B=$ROOT/opt/amix
mkdir -p "$B/man/man1" "$B/doc/infozip"
(cd "$Z" && cp zip zipnote zipsplit zipcloak "$B/bin/" && cp man/zip*.1 "$B/man/man1/")
(cd "$U" && cp unzip funzip unzipsfx "$B/bin/" && cp unix/zipgrep "$B/bin/" &&
	cp man/unzip.1 man/funzip.1 man/unzipsfx.1 man/zipinfo.1 man/zipgrep.1 "$B/man/man1/")
printf '#!/bin/sh\nexec /opt/amix/bin/unzip -Z "$@"\n' > "$B/bin/zipinfo"
chmod 755 "$B/bin/zipgrep" "$B/bin/zipinfo"
cp "$Z/LICENSE" "$B/doc/infozip/"
portpkg
