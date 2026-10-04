#!/bin/sh
# build.sh -- fVDI and its driver for the container's frame buffer.
#
#   sh kernel/guest/tos/fvdi/build.sh outdir
#
# Out: outdir/{fvdi.prg,ashfb.sys,fvdi.sys}.  FVDI names the fVDI source
# tree (default ref/fvdi; cloned from github.com/th-otto/fvdi, GPL, when
# absent).  Built with the gcc 13 m68k cross compiler as TOS programs:
# each image is linked at 0 and 0x10000 and the difference gives the
# relocations.
set -e
H=$(cd "$(dirname "$0")" && pwd)
AUX=${AUX:-$(cd "$H/../../../.." && pwd)}
OUT=$1
FVDI=${FVDI:-$AUX/ref/fvdi}
FVDIREV=f40ae23
B=$AUX/toolchain/linux/bin
[ -d "$FVDI" ] || { git clone -q https://github.com/th-otto/fvdi.git "$FVDI" &&
	git -C "$FVDI" checkout -q $FVDIREV; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
W=$OUT/work
rm -rf "$W"
mkdir -p "$W/bin"
cp -r "$FVDI" "$W/src"
patch -s -p1 -d "$W/src" < "$H/wheelv.patch"
cat > "$W/bin/cc" <<CC
#!/bin/sh
exec nice -n 19 $B/m68k-linux-gnu-gcc -m68020 -Wa,--register-prefix-optional \\
	-fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \\
	-fno-builtin -ffreestanding -fleading-underscore -I$H/inc -I$H/.. \\
	-U__linux__ -U__linux -Ulinux -U__unix__ -U__unix -Uunix -D__CDECL= -D__MINT__ "\$@"
CC
chmod +x "$W/bin/cc"
# a TOS program from ELF objects
prg() {
	o=$1
	shift
	for base in 0 0x10000; do
		"$B/m68k-linux-gnu-ld" -N -e 0 -Ttext=$base --defsym=__end=_end -z noexecstack \
			-o "$W/$base.elf" "$@"
		"$B/m68k-linux-gnu-objcopy" -O binary "$W/$base.elf" "$W/$base.bin"
		[ $(($(wc -c < "$W/$base.bin") % 2)) = 0 ] || printf '\0' >> "$W/$base.bin"
	done
	end=$("$B/m68k-linux-gnu-nm" "$W/0.elf" | awk '$3 == "_end" { print $1 }')
	bss=$((0x$end - $(wc -c < "$W/0.bin")))
	[ $bss -ge 0 ] || bss=0
	python3 "$AUX/tests/tos/elf2prg.py" "$W/0.bin" "$W/0x10000.bin" $bss "$o"
}
mk() {
	PATH=$B:$PATH nice -n 19 make -s -j1 -C "$1" M68K_ATARI_MINT_CROSS=yes \
		CROSSPREFIX=m68k-linux-gnu CC="$W/bin/cc" LD=echo CPU=020 "$2" > "$W/mk.log" 2>&1 ||
		{ tail -20 "$W/mk.log" >&2; exit 1; }
}
# the engine: make compiles, we link
mk "$W/src/engine" fvdi_gnu.prg
objs=$(sed -n 's/^-o fvdi_gnu.prg -Map=fvdi.map //p' "$W/mk.log" | sed 's/ -s .*//')
(cd "$W/src/engine" && prg "$OUT/fvdi.prg" $objs)
# the driver: fVDI's common parts and ours
mk "$W/src/drivers/16_bit" ../common/c_common.gnu.o
mk "$W/src/drivers/16_bit" ../common/common.gnu.o
mk "$W/src/drivers/16_bit" ../common/clip.gnu.o
D=$W/src/drivers/common
CF="-O2 -fomit-frame-pointer -fno-common -Wall -I$W/src/include -I$W/src/drivers/include -I$W/src/modules/include"
for c in init colours printf; do
	"$W/bin/cc" $CF -c "$D/$c.c" -o "$W/$c.o"
done
"$W/bin/cc" $CF -c "$H/ashfb.c" -o "$W/ashfb.o"
prg "$OUT/ashfb.sys" "$D/c_common.gnu.o" "$D/common.gnu.o" "$D/clip.gnu.o" \
	"$W/init.o" "$W/colours.o" "$W/printf.o" "$W/ashfb.o"
sed 's/$/\r/' "$H/fvdi.sys" > "$OUT/fvdi.sys"
rm -rf "$W"
echo "[ok] fvdi.prg, ashfb.sys, fvdi.sys"
