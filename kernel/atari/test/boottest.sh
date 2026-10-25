#!/bin/sh
# boottest.sh -- host test of instboot.py on a disk shared with TOS: a
# bootable BGM partition, AXB, ICD entries and a bad-sector list.  The
# install must keep every byte outside the boot code and checksum and
# outside AXB, and give a sector the ROM runs (word sum 0x1234).
# mkgemboot.py: a FAT16 partition in the ICD slot whose \AUTO\AXBOOT.PRG
# is the launcher; the root sector stays bootable, a used tail is refused.
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

# EmuTOS's GEMDOS partition in the free last 3 MB
python3 - "$T/gem.img" <<'EOF'
import struct, sys
s = bytearray(512)
struct.pack_into('>I', s, 0x1C2, 16384)
struct.pack_into('>B3sII', s, 0x1C6, 0x01, b'AXB', 64, 1024)
struct.pack_into('>B3sII', s, 0x1D2, 0x01, b'AXR', 1088, 9000)
open(sys.argv[1], 'wb').write(s + bytes(16383 * 512))
EOF
python3 "$A/instboot.py" "$T/gem.img" "$T/bootsec.bin" "$T/axbload.bin" "$T/kernel" 'root=c?d0s1' > /dev/null
cp "$T/gem.img" "$T/gem0.img"
python3 "$A/mkgemboot.py" "$T/gem.img" "$T/AXBOOT.PRG"
python3 - "$T/gem0.img" "$T/gem.img" "$T/AXBOOT.PRG" "$T/part.img" <<'EOF'
import struct, sys
a, b, prg = (open(f, 'rb').read() for f in sys.argv[1:4])
assert sum(struct.unpack('>256H', b[:512])) & 0xFFFF == 0x1234, 'root sector not bootable'
assert a[:0x156] == b[:0x156] and a[0x1C2:0x1FE] == b[0x1C2:0x1FE], 'boot code or primaries changed'
assert a[512:(16384 - 6144) * 512] == b[512:(16384 - 6144) * 512], 'partitions changed'
flg, pid, st, n = struct.unpack_from('>B3sII', b, 0x156)
assert (flg & 1, pid, st, n) == (1, b'GEM', 16384 - 6144, 6144), 'ICD entry'
p = b[st * 512:(st + n) * 512]
open(sys.argv[4], 'wb').write(p)
bps, spc, res, nf, nroot, _, _, fsz = struct.unpack_from('<HBHBHHBH', p, 11)
root = (res + nf * fsz) * 512
data = root + nroot * 32
def find(d, name):
	for i in range(0, 512, 32):
		if d[i:i + 11] == name:
			return struct.unpack_from('<HI', d, i + 26)
	raise SystemExit('[FAIL] no %s' % name)
cl, _ = find(p[root:root + nroot * 32], b'AUTO       ')
cl, size = find(p[data + (cl - 2) * 512:], b'AXBOOT  PRG')
fat = p[res * 512:(res + fsz) * 512]
got = b''
while cl < 0xFFF8:
	got += p[data + (cl - 2) * 512:data + (cl - 1) * 512]
	cl, = struct.unpack_from('<H', fat, 2 * cl)
assert got[:size] == prg and size == len(prg), 'AXBOOT.PRG content'
assert prg[:2] == b'\x60\x1a', 'not a TOS program'
print('[OK] GEMDOS partition holds the launcher; root sector bootable, partitions kept')
EOF
! command -v fsck.fat > /dev/null || fsck.fat -n "$T/part.img" > /dev/null
python3 -c "import struct, sys
f = open(sys.argv[1], 'r+b'); s = bytearray(f.read(512))
struct.pack_into('>I', s, 0x1D6 + 4, 16384 - 1088); f.seek(0); f.write(s)" "$T/gem0.img"
if python3 "$A/mkgemboot.py" "$T/gem0.img" "$T/AXBOOT.PRG" > /dev/null; then
	echo '[FAIL] used tail taken'; exit 1
fi
echo '[OK] used tail refused'
