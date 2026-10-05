#!/bin/sh
# mktos.sh -- the TOS container's files for the disk image: starttos,
# maketos, tosdrive, EmuTOS, the machine-layer cartridge, the C: folder
# template /tos/sys, guest's copy of it and the system apps (drive G:).
#
#   sh images/tosenv/mktos.sh outdir
#
# Out: outdir/{starttos,maketos,tosdrive,emutos.img,tosml.img} and
# outdir/{sys,guest,games}.cpio, rooted at /.  EMUTOS names the EmuTOS
# 512 KB release zip, EMUTOSLANG its image (us); FVDI the fVDI tree
# (kernel/guest/tos/fvdi/build.sh).  TOSAPPS names the
# unpacked apps of APPS.md (default ref/tosapps); without them the
# image has none.  NOTERADESK=1 leaves TeraDesk out,
# NOFVDI=1 fVDI.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
G=$AUX/kernel/guest
EMUTOS=${EMUTOS:-$AUX/ref/emutos-release/emutos-512k-1.4.zip}
TOSAPPS=${TOSAPPS:-$AUX/ref/tosapps}
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
BIN=$AUX/toolchain/bin
OUT=$1
[ -n "$OUT" ] || { echo "usage: mktos.sh outdir"; exit 1; }
O=$OUT/obj
rm -rf "$OUT"
mkdir -p "$O/extra"
unzip -p "$EMUTOS" "*/etos512${EMUTOSLANG:-us}.img" > "$OUT/emutos.img"
[ -s "$OUT/emutos.img" ] || { echo "[FAIL] no EmuTOS image in $EMUTOS"; exit 1; }
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
# nonshared libc members
(cd "$O/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
 ar rc ../libextra.a $(ar t "$SYS/usr/ccs/lib/libc.so" | grep -v '^libc.so.1$'))
for c in starttos tosdisp; do
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 \
		-I"$G/mod/tosguest" -I"$AUX/kernel/mac/display" -c "$G/tos/$c.c" -o "$O/$c.o"
done
"$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/starttos" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$O/starttos.o" "$O/tosdisp.o" "$SYS/usr/lib/libc.so.1" \
	"$O/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
# cartridge: machine layer, the U: drive and STiK
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -m68020 -Wall -Wno-implicit -fno-builtin \
	-c "$G/tos/hostfs.c" -o "$O/hostfs.o"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -m68020 -Wall -Wno-implicit -fno-builtin \
	-I"$G/mod/tosguest" -c "$G/tos/stik.c" -o "$O/stik.o"
"$BIN/m68k-elf-as" -m68040 --register-prefix-optional -o "$O/tosml.o" "$G/tos/tosml.s"
"$BIN/m68k-elf-ld" --no-warn-rwx-segments -N -Ttext=0xfa0000 -o "$O/tosml.elf" "$O/tosml.o" "$O/hostfs.o" "$O/stik.o"
end=$("$BIN/m68k-elf-nm" "$O/tosml.elf" | awk '$3 == "_end" { print $1 }')
[ $((0x$end)) -le $((0xfa0000 + 0x20000)) ] || { echo "[FAIL] cartridge ends at $end" >&2; exit 1; }
"$BIN/m68k-elf-objcopy" -O binary "$O/tosml.elf" "$OUT/tosml.img"
cp "$D/maketos" "$D/tosdrive" "$OUT/"
# /tos/sys: README.TXT, AUTO, the user's apps (C:\APPS), G: in the drive table
T=$O/root
mkdir -p "$T/tos/sys/AUTO" "$T/tos/sys/APPS" "$T/usr/games/tos"
sed 's/$/\r/' "$D/c-readme.txt" > "$T/tos/sys/README.TXT"
printf '# drive letter, host directory, ro: read-only\nG /usr/games/tos ro\n' > "$T/tos/sys/drives"
if [ -d "$TOSAPPS/qed/qed" ]; then
	mkdir -p "$T/tos/sys/APPS/QED"
	(cd "$TOSAPPS/qed/qed" && cp -r qed.app qed.rsc icons.rsc qed.cfg readme.txt syntax kurzel \
		"$T/tos/sys/APPS/QED/")
else
	echo "[warn] no $TOSAPPS/qed: no C:\\APPS\\QED"
fi
if [ -d "$TOSAPPS/baller" ]; then
	mkdir -p "$T/usr/games/tos/BALLER"
	cp "$TOSAPPS"/baller/BALLER.* "$T/usr/games/tos/BALLER/"
else
	echo "[warn] no $TOSAPPS/baller: drive G: empty"
fi
# fVDI: GEM drawn into the session's frame buffer; its source beside it (GPL)
if AUX=$AUX sh "$G/tos/fvdi/build.sh" "$O/fvdi" > "$O/fvdi.log" 2>&1; then
	cp "$O/fvdi/fvdi.sys" "$T/tos/sys/FVDI.SYS"
	cp "$O/fvdi/ashfb.sys" "$T/tos/sys/ASHFB.SYS"
	cp "$O/fvdi/fvdi.prg" "$T/tos/sys/AUTO/FVDI.PRG"
	mkdir -p "$T/tos/src/fvdi/ashfb"
	tar -C "$(dirname "${FVDI:-$AUX/ref/fvdi}")" --exclude=.git -czf "$T/tos/src/fvdi/fvdi.tar.gz" \
		"$(basename "${FVDI:-$AUX/ref/fvdi}")"
	cp -r "$G/tos/fvdi/." "$G/tos/tosfb.h" "$T/tos/src/fvdi/ashfb/"
elif [ "$NOFVDI" = 1 ]; then
	echo "[warn] fVDI (NOFVDI=1): GEM uses the ROM's VDI"
else
	echo "[FAIL] fVDI: $(tail -1 "$O/fvdi.log") (NOFVDI=1 to build without it)"; exit 1
fi
# TeraDesk, a desktop to run instead of the ROM's; its source beside it (GPL)
if [ -n "$NOTERADESK" ]; then
	echo "[warn] TeraDesk left out"
elif AUX=$AUX MINT=$O/mint sh "$G/tos/teradesk/build.sh" "$O/teradesk" > "$O/teradesk.log" 2>&1; then
	mkdir -p "$T/tos/src"
	cp -r "$O/teradesk/TERADESK" "$T/tos/sys/"
	cp "$O/teradesk/teradesk.tar.gz" "$T/tos/src/"
	# EmuDesk's saved desktop: TeraDesk starts at boot, text files open in Qed
	if [ -d "$T/tos/sys/APPS/QED" ]; then cat "$D/emudesk.inf"; else grep -v QED "$D/emudesk.inf"; fi |
		sed 's/$/\r/' > "$T/tos/sys/EMUDESK.INF"
else
	echo "[FAIL] TeraDesk: $(tail -1 "$O/teradesk.log"); NOTERADESK=1 builds without it"
	exit 1
fi
# guest's TOS folder, as maketos makes it
mkdir -p "$T/home/guest"
(cd "$T/tos/sys" && find . | grep -v -x ./drives | cpio -pdm --quiet "$T/home/guest/TOS")
find "$T" -type d -exec chmod 755 {} +
find "$T" -type f -exec chmod 644 {} +
(cd "$T" && find tos | cpio -o -H newc -R 0:3 --quiet) > "$OUT/sys.cpio"
(cd "$T" && find usr/games | cpio -o -H newc -R 0:3 --quiet) > "$OUT/games.cpio"
(cd "$T" && find home/guest/TOS | cpio -o -H newc -R 100:1 --quiet) > "$OUT/guest.cpio"
rm -rf "$O"
echo "[ok] starttos, EmuTOS, cartridge, /tos/sys, apps"
