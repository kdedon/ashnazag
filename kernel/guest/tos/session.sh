#!/bin/sh
# session.sh out -- SESSION.ACC, the Desk menu's session accessory.
set -e
D=$(cd "$(dirname "$0")" && pwd)
BIN=$(cd "$D/../../.." && pwd)/toolchain/bin
W=$(mktemp -d "$(dirname "$1")/.acc.XXXXXX")
trap 'rm -rf "$W"' EXIT HUP INT TERM
"$BIN/m68k-elf-as" -m68040 --register-prefix-optional -o "$W/s.o" "$D/session.s"
for b in 0 0x10000; do
	"$BIN/m68k-elf-ld" --no-warn-rwx-segments -N -Ttext=$b -o "$W/s$b.elf" "$W/s.o"
	"$BIN/m68k-elf-objcopy" -O binary -j .text "$W/s$b.elf" "$W/s$b.bin"
done
# the same at any address: a header, the text and no relocations
cmp -s "$W/s0.bin" "$W/s0x10000.bin" || { echo "session.sh: not position-independent" >&2; exit 1; }
python3 -c '
import struct, sys
t = open(sys.argv[1], "rb").read()
t += b"\0" * (len(t) & 1)
open(sys.argv[2], "wb").write(struct.pack(">HIIIIIIH", 0x601a, len(t), 0, 0, 0, 0, 0, 0) + t + bytes(4))
' "$W/s0.bin" "$1"
