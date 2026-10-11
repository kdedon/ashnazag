#!/usr/bin/env python3
# patch_tt.py -- the low-gigabyte transparent translation for the
# supervisor only, as on the Mac host: user space below 1 GB (an
# environment's guest memory) then goes through the page tables.
#
#   pstart  movel #0x003fc000,%d0; movec %d0,%itt0  -> #0x003fa000
#           movel #0x003fc060,%d0; movec %d0,%dtt0  -> #0x003fa060
#
# usage: patch_tt.py [-c] image   (-c: check only, exit 1 unless converted)
import sys

args = sys.argv[1:]
check = args[:1] == ['-c']
if check:
	args = args[1:]
d = bytearray(open(args[0], 'rb').read())
SITES = [('203c003fc0004e7b0004', '203c003fa0004e7b0004'),
	('203c003fc0604e7b0006', '203c003fa0604e7b0006')]
for old, new in SITES:
	old, new = bytes.fromhex(old), bytes.fromhex(new)
	n, m = d.count(old), d.count(new)
	if check:
		if n or m != 1:
			sys.exit('patch_tt: %s not converted' % new.hex())
		continue
	if m == 1 and n == 0:
		continue
	if n != 1 or m:
		sys.exit('patch_tt: expected one %s, found %d' % (old.hex(), n))
	i = d.index(old)
	d[i:i + len(new)] = new
if not check:
	open(args[0], 'wb').write(d)
