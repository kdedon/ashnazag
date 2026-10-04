#!/bin/sh
# boottest.sh -- host test of instboot.py on a disk shared with TOS: a
# bootable BGM partition, AXB, ICD entries and a bad-sector list.  The
# install must keep every byte outside the boot code and checksum and
# outside AXB, and give a sector the ROM runs (word sum 0x1234).
set -e
A=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/boottest.XXXXXX")
trap 'rm -rf "$T"' EXIT
sh "$A/mkboot.sh" "$T"
python3 - "$T/kernel" <<'EOF'
import os, struct, sys
k = bytearray(os.urandom(300001))
k[:52] = struct.pack('>4s5B7xHHIIIIIHHHHHH', b'\x7fELF', 1, 2, 1, 0, 0,
                     2, 4, 1, 0x1000, 52, 0, 0, 52, 32, 1, 40, 0, 0)
k[52:84] = struct.pack('>8I', 1, 0x1000, 0x1000, 0x1000, 290001, 400000, 7, 0x1000)
open(sys.argv[1], 'wb').write(k)
EOF
python3 - "$T/disk.img" <<'EOF'
import struct, sys
s = bytearray(512)
s[:0x156] = b'\x4e\x75' + bytes(0x154)		# some other boot code
s[0x156:0x1C2] = bytes(range(0x6C))		# ICD entries
struct.pack_into('>I', s, 0x1C2, 4096)
struct.pack_into('>B3sII', s, 0x1C6, 0x81, b'BGM', 16, 1024)
struct.pack_into('>B3sII', s, 0x1D2, 0x01, b'AXB', 1040, 1024)
struct.pack_into('>II', s, 0x1F6, 2064, 1)		# bad-sector list
img = bytearray(4096 * 512)
img[:512] = s
for i in range(512, len(img), 512):
	img[i:i + 4] = struct.pack('>I', i // 512)
open(sys.argv[1], 'wb').write(img)
EOF
cp "$T/disk.img" "$T/orig.img"
python3 "$A/instboot.py" "$T/disk.img" "$T/bootsec.bin" "$T/axbload.bin" "$T/kernel" 'root=c?d0s1' 5
python3 "$A/instboot.py" --check "$T/disk.img" "$T/bootsec.bin" "$T/axbload.bin" "$T/kernel" 'root=c?d0s1'
python3 - "$T/orig.img" "$T/disk.img" <<'EOF'
import sys
a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
assert len(a) == len(b)
axb = range(1040 * 512, 2064 * 512)
bad = [i for i in range(len(a)) if a[i] != b[i]
       and not (i < 0x156 or 0x1FE <= i < 0x200 or i in axb)]
assert not bad, 'changed outside boot code and AXB: %#x' % bad[0]
assert a[0x156:0x1FE] == b[0x156:0x1FE]
print('[OK] partition table, ICD entries, bad-sector list and other partitions kept')
EOF
# the old boot code comes back; layouts that overlap or leave the disk are refused
python3 "$A/instboot.py" --restore "$T/disk.img" "$T/disk.img.rootsec"
cmp -n 512 "$T/orig.img" "$T/disk.img"
for bad in 's[0x1D6:0x1DA] = struct.pack(">I", 1030)' 's[0x1C2:0x1C6] = struct.pack(">I", 8192)' \
	's[0x1F6:0x1FA] = struct.pack(">I", 1050)'; do
	cp "$T/orig.img" "$T/bad.img"
	python3 -c "import struct, sys
f = open(sys.argv[1], 'r+b'); s = bytearray(f.read(512)); $bad; f.seek(0); f.write(s)" "$T/bad.img"
	cp "$T/bad.img" "$T/bad0.img"
	if python3 "$A/instboot.py" "$T/bad.img" "$T/bootsec.bin" "$T/axbload.bin" \
		"$T/kernel" 'root=c?d0s1'; then echo '[FAIL] bad layout taken'; exit 1; fi
	cmp "$T/bad.img" "$T/bad0.img"
done
echo '[OK] bad layouts refused and left alone; old boot code restored'
