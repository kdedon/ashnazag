#!/bin/sh
# build.sh dir: the SndTest INIT as dir/SndTest and dir/%SndTest (AppleDouble).
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
LX=$AUX/toolchain/linux/bin/m68k-linux-gnu
W=$(mktemp -d)
"$LX-as" -m68020 -o "$W/test.o" "$D/test.s"
"$LX-gcc" -m68020 -O2 -mpcrel -fomit-frame-pointer -ffreestanding -fno-builtin \
	-fno-asynchronous-unwind-tables -Wall -Werror -c -o "$W/sndtest.o" "$D/sndtest.c"
cat > "$W/ld.x" <<'X'
SECTIONS {
	. = 0;
	.text : { *test.o(.text) *(.text*) *(.rodata*) *(.data*) *(.bss*) *(COMMON) _end = .; }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
X
"$LX-ld" -T "$W/ld.x" --build-id=none -z noexecstack -o "$W/init.elf" "$W/test.o" "$W/sndtest.o"
if "$LX-objdump" -dr "$W/sndtest.o" | grep -q 'R_68K_32'; then
	echo "build.sh: absolute references in sndtest.o"; exit 1
fi
"$LX-objcopy" -O binary -j .text "$W/init.elf" "$W/init.bin"
python3 - "$AUX/tools" "$W/init.bin" "$1" <<'P'
import sys, os, struct
sys.path.insert(0, sys.argv[1])
import macbin2ad
code, out = open(sys.argv[2], 'rb').read(), sys.argv[3]
data = struct.pack('>I', len(code)) + code
tl = struct.pack('>H', 0) + b'INIT' + struct.pack('>HH', 0, 10)
rl = struct.pack('>hhI', 128, -1, 0x10 << 24) + bytes(4)
m = bytes(24) + struct.pack('>HH', 28, 28 + len(tl) + len(rl)) + tl + rl
f = struct.pack('>IIII', 256, 256 + len(data), len(data), len(m)) + bytes(240) + data + m
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'SndTest'), 'wb').close()
open(os.path.join(out, '%SndTest'), 'wb').write(macbin2ad.ad_header(b'INITaSNT', len(f)) + f)
P
rm -rf "$W"
