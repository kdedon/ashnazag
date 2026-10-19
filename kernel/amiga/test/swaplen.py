#!/usr/bin/env python3
# swaplen.py -- set the length of an AMIX kernel's boot swap area, in
# 512-byte blocks (stock: 61440, 30 MB), to fill a bigger swap partition.
# Its page slots come from the kernel heap, which grows as needed.
#
#   swaplen.py kernel MB
import struct, sys

f, mb = sys.argv[1], int(sys.argv[2])
b = bytearray(open(f, 'rb').read())
shoff, = struct.unpack_from('>I', b, 32)
shentsize, shnum = struct.unpack_from('>HH', b, 46)
sh = [struct.unpack_from('>IIIIIIIIII', b, shoff + i * shentsize) for i in range(shnum)]
sym = next(s for s in sh if s[1] == 2)
strtab = sh[sym[6]]
for o in range(sym[4], sym[4] + sym[5], 16):
	nm, val, size, info, other, shndx = struct.unpack_from('>IIIBBH', b, o)
	s = strtab[4] + nm
	if b[s:b.index(0, s)] == b'swapfile':
		break
else:
	sys.exit('swaplen: no swapfile')
# swapconf passes swapadd the start at +0x94 and the length at +0x98
p = sh[shndx][4] + val - sh[shndx][3] + 0x98
if struct.unpack_from('>I', b, p - 4) != (0,) or not b[p - 0x88:p - 0x87] == b'/':
	sys.exit('swaplen: unexpected swapfile layout')
struct.pack_into('>I', b, p, mb * 2048)
open(f, 'wb').write(b)
