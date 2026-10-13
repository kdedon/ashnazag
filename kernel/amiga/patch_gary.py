#!/usr/bin/env python3
# patch_gary.py -- config() sets the A3000 bus timeout at 0xDE0002 on any
# machine; the kernel entry does it on the A3000 only.
#
#   config  orib #0x80,0xde0002  ->  nop x4
#
# usage: patch_gary.py [-c] image   (-c: check only, exit 1 unless converted)
import sys

args = sys.argv[1:]
check = args[:1] == ['-c']
if check:
	args = args[1:]
d = bytearray(open(args[0], 'rb').read())
old, new = bytes.fromhex('0039008000de0002'), bytes.fromhex('4e714e714e714e71')
n = d.count(old)
if check:
	if n:
		sys.exit('patch_gary: not converted')
	sys.exit(0)
if n == 1:
	i = d.index(old)
	d[i:i + len(new)] = new
	open(args[0], 'wb').write(d)
elif n:
	sys.exit('patch_gary: expected one site, found %d' % n)
