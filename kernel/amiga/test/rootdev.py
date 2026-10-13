#!/usr/bin/env python3
# rootdev.py -- point an AMIX kernel's root and swap at another sd
# card and unit: rootdev and the swap path, whose controller digit is
# card * 8 + unit in hex (card 1 unit 6: ced0; unit 0: c8d0).
#
#   rootdev.py kernel card [unit]
import struct, sys

f, card = sys.argv[1], int(sys.argv[2])
unit = int(sys.argv[3]) if len(sys.argv) > 3 else None
b = bytearray(open(f, 'rb').read())
shoff, = struct.unpack_from('>I', b, 32)
shentsize, shnum = struct.unpack_from('>HH', b, 46)
sh = [struct.unpack_from('>IIIIIIIIII', b, shoff + i * shentsize) for i in range(shnum)]
sym = next(s for s in sh if s[1] == 2)
strtab = sh[sym[6]]

def find(name):
	for o in range(sym[4], sym[4] + sym[5], 16):
		nm, val, size, info, other, shndx = struct.unpack_from('>IIIBBH', b, o)
		s = strtab[4] + nm
		if b[s:b.index(0, s)].decode() == name:
			return sh[shndx][4] + val - sh[shndx][3]
	sys.exit('rootdev: no ' + name)

r = find('rootdev')
dev, = struct.unpack_from('>I', b, r)
if dev >> 18 != 18:
	sys.exit('rootdev: rootdev is not sd')
if unit is not None:
	dev = dev & ~7 | unit
struct.pack_into('>I', b, r, dev & ~8 | card << 3)
s = find('swapfile') + 16
p = b[s:b.index(0, s)].decode()
if not p.startswith('/dev/dsk/c') or len(p) != 15:
	sys.exit('rootdev: swap is ' + p)
b[s + 10] = ord('%x' % (card * 8 + (dev & 7)))
open(f, 'wb').write(b)
