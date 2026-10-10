#!/usr/bin/env python3
# rdb.py -- an Amiga hard disk image (RDB) holding AMIX partitions.
#
#   rdb.py out.hdf NAME:DOSTYPE:MB[:IMAGE] ...
#
# Partitions follow in order from cylinder 2, after the RDB; AMIX numbers
# them s1, s2, ...  DOSTYPE: four characters, \N for a byte (UNI\1 root,
# resv swap, UNI\0 boot).  IMAGE fills the partition from its start.
# Geometry: 32 sectors x 16 heads, 256 KB cylinders.
import struct, sys

SECS, HEADS, BS = 32, 16, 512
CYL = SECS * HEADS

def csum(b):
	n = struct.unpack('>I', b[4:8])[0]
	s = sum(struct.unpack('>%dI' % n, b[:n * 4])) & 0xffffffff
	return b[:8] + struct.pack('>I', -s & 0xffffffff) + b[12:]

def dostype(s):
	out = b''
	i = 0
	while i < len(s):
		if s[i] == '\\':
			out += bytes((int(s[i + 1]),))
			i += 2
		else:
			out += s[i].encode()
			i += 1
	if len(out) != 4:
		sys.exit('rdb.py: bad dostype %s' % s)
	return struct.unpack('>I', out)[0]

def main():
	if len(sys.argv) < 3:
		sys.exit('usage: rdb.py out.hdf NAME:DOSTYPE:MB[:IMAGE] ...')
	parts, cyl = [], 2
	for a in sys.argv[2:]:
		f = a.split(':', 3)
		ncyl = int(f[2]) * 1024 * 1024 // (CYL * BS)
		parts.append((f[0], dostype(f[1]), cyl, cyl + ncyl - 1, f[3] if len(f) > 3 else None))
		cyl += ncyl
	ncyls = cyl
	rdsk = bytearray(BS)
	struct.pack_into('>4sIIIIIiiii', rdsk, 0, b'RDSK', 64, 0, 7, BS, 0, -1, 1, -1, -1)
	struct.pack_into('>6i', rdsk, 40, *[-1] * 6)
	struct.pack_into('>IIIII', rdsk, 64, ncyls, SECS, HEADS, 1, ncyls)
	struct.pack_into('>III', rdsk, 96, ncyls, ncyls, 3)
	struct.pack_into('>IIIIIII', rdsk, 128, 0, 2 * CYL - 1, 2, ncyls - 1, CYL, 0, len(parts))
	rdsk[160:168] = b'AMIX    '
	rdsk[168:184] = b'ROOT DISK       '
	rdsk[184:188] = b'1.0 '
	blocks = [bytes(csum(bytes(rdsk)))]
	for i, (name, dt, lo, hi, img) in enumerate(parts):
		p = bytearray(BS)
		nxt = i + 2 if i + 1 < len(parts) else -1
		struct.pack_into('>4sIIIiI', p, 0, b'PART', 64, 0, 7, nxt, 0)
		p[36] = len(name)
		p[37:37 + len(name)] = name.encode()
		env = (16, BS // 4, 0, HEADS, 1, SECS, 2, 0, 0, lo, hi, 30, 0, 0x7fffffff,
			0xfffffffe, 0, dt)
		struct.pack_into('>17I', p, 128, *env)
		blocks.append(bytes(csum(bytes(p))))
	with open(sys.argv[1], 'wb') as o:
		o.truncate(ncyls * CYL * BS)
		for i, b in enumerate(blocks):
			o.seek(i * BS)
			o.write(b)
		for name, dt, lo, hi, img in parts:
			if not img:
				continue
			d = open(img, 'rb').read()
			if len(d) > (hi - lo + 1) * CYL * BS:
				sys.exit('rdb.py: %s does not fit %s' % (img, name))
			o.seek(lo * CYL * BS)
			o.write(d)

main()
