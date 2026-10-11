#!/usr/bin/env python3
# namelist.py -- /stand/unix for nlist (netstat, arp, crash): the kernel
# linked at the load base, its symbols where the loader put them.
#
#   namelist.py kernel linked out
#
# kernel: the relocatable boot kernel; linked: it linked at the base.
# The loader puts text, data and bss in a row with no padding, and gives
# each common symbol, in symbol-table order, its own alignment (st_value)
# at the end of bss; the link aligns bss and orders commons its own way.
import struct, sys

def elf(path):
	d = bytearray(open(path, 'rb').read())
	shoff = struct.unpack_from('>I', d, 32)[0]
	esz, nsh, stri = struct.unpack_from('>HHH', d, 46)
	sh = [struct.unpack_from('>10I', d, shoff + i * esz) for i in range(nsh)]
	names = d[sh[stri][4]:]
	secs = {bytes(names[s[0]:names.index(b'\0', s[0])]).decode(): i for i, s in enumerate(sh)}
	sym = [s for s in sh if s[1] == 2][0]
	strs = d[sh[sym[6]][4]:]
	syms = []
	for k in range(sym[5] // 16):
		o = sym[4] + k * 16
		n, value, size = struct.unpack_from('>III', d, o)
		shn = struct.unpack_from('>H', d, o + 14)[0]
		syms.append((bytes(strs[n:strs.index(b'\0', n)]).decode(), o, value, size, shn))
	return d, sh, secs, syms

rd, rsh, rsecs, rsyms = elf(sys.argv[1])
ld, lsh, lsecs, lsyms = elf(sys.argv[2])
text, data, bss = (lsh[lsecs[n]][3] for n in ('.text', '.data', '.bss'))
lbss = text + rsh[rsecs['.text']][5] + rsh[rsecs['.data']][5]
if data != text + rsh[rsecs['.text']][5]:
	sys.exit('namelist.py: data does not follow text')
end = lbss + rsh[rsecs['.bss']][5]
common = {}
for name, o, value, size, shn in rsyms:
	if shn == 0xfff2:
		end = (end + value - 1) & ~(value - 1)
		common[name] = end
		end += size
for name, o, value, size, shn in lsyms:
	if name in common:
		struct.pack_into('>I', ld, o + 4, common[name])
	elif shn == lsecs['.bss']:
		struct.pack_into('>I', ld, o + 4, value - bss + lbss)
open(sys.argv[3], 'wb').write(ld)
