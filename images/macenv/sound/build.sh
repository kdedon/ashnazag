#!/bin/sh
# build.sh dir: the SoundOut extension as dir/SoundOut and dir/%SoundOut.
set -e
D=$(cd "$(dirname "$0")" && pwd)
LX=$D/../../../toolchain/linux/bin/m68k-linux-gnu
W=$(mktemp -d)
"$LX-as" -m68020 -o "$W/glue.o" "$D/glue.s"
"$LX-gcc" -m68020 -O2 -mpcrel -fomit-frame-pointer -ffreestanding -fno-builtin \
	-fno-asynchronous-unwind-tables -Wall -Werror -c -o "$W/sm.o" "$D/sm.c"
"$LX-gcc" -m68020 -O2 -mpcrel -fomit-frame-pointer -ffreestanding -fno-builtin \
	-fno-asynchronous-unwind-tables -Wall -Werror -c -o "$W/sdev.o" "$D/sdev.c"
cat > "$W/ld.x" <<'X'
SECTIONS {
	. = 0;
	.text : { *glue.o(.text) *(.text*) *(.rodata*) *(.data*) *(.bss*) *(COMMON) _end = .; }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
X
"$LX-ld" -T "$W/ld.x" --build-id=none -z noexecstack -o "$W/init.elf" "$W/glue.o" "$W/sm.o" "$W/sdev.o"
# one section, nothing to relocate: the code runs wherever it is copied
s=$("$LX-readelf" -S "$W/init.elf" | grep -c ' PROGBITS\| NOBITS')
[ "$s" -eq 1 ] || { echo "build.sh: sections other than .text"; exit 1; }
if "$LX-objdump" -dr "$W/sm.o" "$W/sdev.o" | grep -q 'R_68K_32'; then
	echo "build.sh: absolute references in sm.o or sdev.o"; exit 1
fi
"$LX-objcopy" -O binary -j .text "$W/init.elf" "$W/init.bin"
python3 "$D/mkinit.py" "$W/init.bin" "$1"
rm -rf "$W"
