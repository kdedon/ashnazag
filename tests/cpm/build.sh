#!/bin/sh
# build.sh -- t_cpm's inputs: startcpm, DRI's CP/M-68K 1.3 and a test program.
#
#   sh tests/cpm/build.sh outdir
#
# Out: outdir/root/cpm/bin/startcpm, outdir/root/cpm/sys/CPM.SYS,
# outdir/root/cpm/dist/ (P:: the first disk's files, X.68K linked to
# each X.REL, EXIT.68K and MOUNT.68K), outdir/root/tests/cpm/hello.68k.  CPMZIP names DRI's release zip
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
# EXIT.68K: BIOS QUIT (0x7f); MOUNT.68K: BIOS MOUNT (0x7e) with the base page, then BDOS 0
python3 -c "
import struct, sys
def prg(path, t):
    open(path, 'wb').write(struct.pack('>HIIIIIIH', 0x601a, len(t), 0, 0, 0, 0, 0, 0) + t + bytes(len(t)))
prg(sys.argv[1], bytes.fromhex('303c007f72004e43'))
prg(sys.argv[2], bytes.fromhex('222f0004303c007e4e4370004e42'))
" "$R/cpm/dist/EXIT.68K" "$R/cpm/dist/MOUNT.68K"
sh "$T/cpm/links.sh" "$R/cpm/dist"
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
# the BDOS on this host, over a scratch directory
cc -std=gnu89 -O -Wall -w -I"$G/cpm" -o "$O/bdostest" "$T/cpm/bdostest.c" \
	"$G/cpm/bdos3.c" "$G/cpm/con3.c" "$G/cpm/hostfs.c"
rm -rf "$O/bdos"
"$O/bdostest" "$O/bdos"
rm -rf "$O/bdos"
# the CP/M-80 and CP/M-86 interpreters on this host, over DRI's programs when present
S=$G/cpm/shim
Z80C="z80dec z80mnem z80exec z80run z80load z80bdos z80gsx gdpb segclr"
I86C="i86dec i86exec i86load i86bdos i86gsx gdpb segclr"
SHIMCORPUS=${SHIMCORPUS:-$AUX/media/cpmshim}
if [ -d "$SHIMCORPUS/z80corpus" ] && [ -d "$SHIMCORPUS/i86corpus" ]; then
	cc -std=gnu89 -O -w -DHOSTCC -I"$S" -o "$O/z80test" "$T/cpm/shim/z80test.c" \
		$(for c in $Z80C; do echo "$S/$c.c"; done)
	cc -std=gnu89 -O -w -DHOSTCC -I"$S" -o "$O/i86test" "$T/cpm/shim/i86test.c" \
		$(for c in $I86C; do echo "$S/$c.c"; done)
	python3 "$T/cpm/shim/mkcmdfix.py" "$O/cmdfix" > /dev/null
	mkdir -p "$O/shimrun/build"
	(cd "$O/shimrun" && "$O/z80test" "$SHIMCORPUS/z80corpus" | tail -n 1 &&
	 "$O/i86test" "$SHIMCORPUS/i86corpus" "$O/cmdfix" | grep 'checks,') | tee "$O/shim.log"
	! grep -v ' 0 failures' "$O/shim.log"
	rm -rf "$O/shimrun"
	# DRI's programs and the benchmarks through the shims and our BDOS
	cc -std=gnu89 -O -w -no-pie -DHOSTCC -DI86RUN=i86run -I"$G/cpm" -I"$S" -o "$O/shimrun" \
		"$T/cpm/shim/shimrun.c" "$G/cpm/bdos3.c" "$G/cpm/con3.c" "$G/cpm/hostfs.c" \
		"$G/cpm/cpmshim.c" "$G/cpm/cpmshim80.c" "$G/cpm/cpmshim86.c" \
		$(for c in $Z80C $I86C; do echo "$S/$c.c"; done | sort -u)
	D=$O/shimdisk
	rm -rf "$D"; mkdir -p "$D"
	cp "$SHIMCORPUS/z80corpus/DUMP.COM" "$SHIMCORPUS/z80corpus/PIP.COM" "$SHIMCORPUS/i86corpus/PIP.CMD" "$D/"
	printf 'HELLO FROM A:\r\n\032' > "$D/README.TXT"
	python3 "$T/cpm/shim/mkbench.py" "$D"
	(cd "$D" && "$O/shimrun" . DUMP.COM README.TXT) | grep -q '48 45 4C 4C 4F'
	(cd "$D" && "$O/shimrun" . PIP.COM COPY80.TXT=README.TXT && "$O/shimrun" . PIP.CMD COPY86.TXT=README.TXT)
	cmp -n 16 "$D/copy80.txt" "$D/README.TXT" && cmp -n 16 "$D/copy86.txt" "$D/README.TXT"
	(cd "$D" && "$O/shimrun" . BENCH80.COM && "$O/shimrun" . BENCH86.CMD) | grep -c 'BENCH DONE' | grep -q 2
	echo "shimrun: DUMP, PIP (8080, 8086) and both benchmarks over our BDOS"
	rm -rf "$D"
else
	echo "[skip] shim host tests: no $SHIMCORPUS"
fi
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
CC68="nice -n 19 $TC/bin/m68k-cbm-sysv4-gcc -O -Wall -Wno-implicit -D__STDC__=0"
CCS="nice -n 19 $TC/bin/m68k-cbm-sysv4-gcc -O -w -D__STDC__=0"
AS68="nice -n 19 $TC/bin/m68k-cbm-sysv4-as"
link68() {	# out objects...
	o=$1; shift
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$o" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
		"$@" "$SYS/usr/lib/libc.so.1" "$T/build/obj/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
}
for c in startcpm bdos3 con3 hostfs cpmshim cpmshim80 cpmshim86; do
	$CC68 -I"$G/include" -c "$G/cpm/$c.c" -o "$O/$c.o"
done
$CC68 -DI86RUN=i86run -I"$G/include" -c "$G/cpm/cpmshim86.c" -o "$O/cpmshim86c.o"
$AS68 -o "$O/cpment.o" "$G/cpm/cpment.s"
# the interpreters: C, the 68k fast paths and their tables, and the C references renamed
for c in z80mnem z80exec z80load z80bdos z80gsx gdpb i86dec i86exec i86load i86bdos i86gsx z80dec z80run segclr; do
	$CCS -I"$S" -c "$S/$c.c" -o "$O/s_$c.o"
done
$CCS -Dz80dec=z80decc -I"$S" -c "$S/z80dec.c" -o "$O/s_z80decc.o"
$CCS -Di86dec=i86decc -I"$S" -c "$S/i86dec.c" -o "$O/s_i86decc.o"
ZREN="-Dz80step=cz80step -Dz80exec=cz80exec -Dz80run=cz80run -Dz80flags=cz80flags \
	-Dz80lcond=cz80lcond -Dz80delay=cz80delay -Dz80hookno=cz80hookno -Dz80ninsn=cz80ninsn \
	-Dz80nflag=cz80nflag -Dz80fast=cz80fast -Dz80nskip=cz80nskip -Dz80wait=cz80wait \
	-Dz80rb=cz80rb -Dz80wb=cz80wb -Dz80rw=cz80rw -Dz80ww=cz80ww -Dz80getr=cz80getr \
	-Dz80setr=cz80setr -Dz80dec=z80decc"
$CCS $ZREN -I"$S" -c "$S/z80exec.c" -o "$O/c_z80exec.o"
$CCS $ZREN -I"$S" -c "$S/z80run.c" -o "$O/c_z80run.o"
cc -std=gnu89 -O -w -I"$S" -o "$O/gentab" "$S/gentab.c" "$S/z80dec.c"
"$O/gentab" z80 > "$O/z80tab.s"
cc -std=gnu89 -O -w -I"$S" -o "$O/gentab86" "$S/gentab86.c"
"$O/gentab86" > "$O/i86tab.s"
for a in z80deca z80runa i86deca i86runa segclra; do
	$AS68 -I"$G" -o "$O/$a.o" "$S/$a.s"
done
$AS68 -o "$O/x86com.o" "$G/x86com/x86com.s"
$AS68 -o "$O/z80tab.o" "$O/z80tab.s"
$AS68 -o "$O/i86tab.o" "$O/i86tab.s"
COMMON="$O/startcpm.o $O/bdos3.o $O/con3.o $O/hostfs.o $O/cpment.o $O/cpmshim.o $O/cpmshim80.o \
	$O/s_z80mnem.o $O/s_z80exec.o $O/s_z80load.o $O/s_z80bdos.o $O/s_z80gsx.o \
	$O/s_gdpb.o $O/s_i86exec.o $O/s_i86load.o $O/s_i86bdos.o $O/s_i86gsx.o"
FAST80="$O/z80deca.o $O/z80runa.o $O/z80tab.o $O/s_z80decc.o $O/segclra.o"
FAST="$FAST80 $O/i86deca.o $O/i86runa.o $O/i86tab.o $O/s_i86decc.o $O/x86com.o"
link68 "$R/cpm/bin/startcpm" $COMMON $FAST "$O/cpmshim86.o"
# the C interpreters alone, for timing against the fast paths
link68 "$R/tests/cpm/startcpm-c" $COMMON "$O/s_z80dec.o" "$O/s_z80run.o" "$O/s_segclr.o" \
	"$O/s_i86dec.o" "$O/cpmshim86c.o"
$CC68 -I"$S" -c "$T/cpm/shim/zdiff.c" -o "$O/zdiff.o"
link68 "$R/tests/cpm/zdiff" "$O/zdiff.o" $FAST80 "$O/s_z80mnem.o" "$O/s_z80exec.o" \
	"$O/c_z80exec.o" "$O/c_z80run.o"
$CC68 -I"$S" -c "$T/cpm/shim/idiff.c" -o "$O/idiff.o"
link68 "$R/tests/cpm/idiff" "$O/idiff.o" "$O/i86deca.o" "$O/i86runa.o" "$O/i86tab.o" "$O/x86com.o" \
	"$O/s_i86decc.o" "$O/s_i86exec.o"
# t_cpm's CP/M-80 and CP/M-86 programs
if [ -d "$SHIMCORPUS/z80corpus" ]; then
	mkdir -p "$R/tests/cpm/shim"
	cp "$SHIMCORPUS/z80corpus/DUMP.COM" "$SHIMCORPUS/z80corpus/PIP.COM" \
		"$SHIMCORPUS/i86corpus/PIP.CMD" "$SHIMCORPUS/i86corpus/STAT.CMD" "$R/tests/cpm/shim/"
	python3 "$T/cpm/shim/mkbench.py" "$R/tests/cpm/shim"
fi
rm -rf "$O/zip" "$O/all" "$O"/*.img
echo "[ok] startcpm, CP/M-68K $(ls "$R/cpm/dist" | wc -l | tr -d ' ') files"
