#!/bin/sh
# build.sh -- t_cpm's inputs: startcpm, DRI's CP/M-68K 1.3 and a test program.
#
#   sh tests/cpm/build.sh outdir
#
# Out: outdir/root/cpm/bin/startcpm, outdir/root/cpm/sys/CPM.SYS,
# outdir/root/cpm/dist/ (the first disk's files, for a new A:),
# outdir/root/tests/cpm/hello.68k.  CPMZIP names DRI's release zip
# (default: media/cpm68k/68kv1_3.zip); without it nothing is built and
# t_cpm skips.  First the file system code is checked on this host.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
G=$KDIR/guest
OUT=$1
CPMZIP=${CPMZIP:-$AUX/media/cpm68k/68kv1_3.zip}
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
rm -rf "$OUT/root" "$OUT/obj"
if [ ! -f "$CPMZIP" ] || [ ! -f "$G/cpm/startcpm.c" ]; then
	echo "[skip] no CP/M-68K release or no startcpm"
	exit 0
fi
O=$OUT/obj
R=$OUT/root
mkdir -p "$O/zip" "$O/all" "$R/cpm/bin" "$R/cpm/sys" "$R/cpm/dist" "$R/tests/cpm"
unzip -q -o "$CPMZIP" -d "$O/zip"
# all disks for the checks below, the first disk's copy of a name winning
for d in 1 2 3 4 5 6 7 8 9; do
	for f in "$O/zip/DISK$d"/*; do
		[ -f "$O/all/$(basename "$f")" ] || cp "$f" "$O/all/"
	done
done
cp "$O/zip/DISK1/CPM.SYS" "$R/cpm/sys/CPM.SYS"
for f in "$O"/zip/DISK1/*; do
	[ "$(basename "$f")" = CPM.SYS ] || cp "$f" "$R/cpm/dist/"
done
# HELLO.68K: print a line (BDOS 9), then BDOS 0
python3 -c "
import struct, sys
msg = b'HELLO FROM CP/M-68K\r\n\$'
t = bytes.fromhex('41fa000e2208303c00094e4242404e42') + msg
t += b'\0' * (len(t) & 1)
sys.stdout.buffer.write(struct.pack('>HIIIIIIH', 0x601a, len(t), 0, 0, 0, 0, 0, 0) + t + b'\0' * len(t))
" > "$R/tests/cpm/hello.68k"
# the file system code, on this host, against an independent reader
cc -std=gnu89 -O -Wall -w -I"$G/cpm" -o "$O/fscheck" "$T/cpm/fscheck.c" "$G/cpm/cpmfs.c"
"$O/fscheck" "$O" "$O"/all/*
python3 "$T/cpm/fscheck.py" "$O" "$R/cpm/dist"
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
for c in startcpm cpmfs; do
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -Wno-implicit -D__STDC__=0 \
		-I"$G/mod/tosguest" -c "$G/cpm/$c.c" -o "$O/$c.o"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-as" -o "$O/cpment.o" "$G/cpm/cpment.s"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$R/cpm/bin/startcpm" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$O/startcpm.o" "$O/cpmfs.o" "$O/cpment.o" \
	"$SYS/usr/lib/libc.so.1" "$T/build/obj/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
rm -rf "$O/zip" "$O/all" "$O"/*.img
echo "[ok] startcpm, CP/M-68K $(ls "$R/cpm/dist" | wc -l | tr -d ' ') files"
