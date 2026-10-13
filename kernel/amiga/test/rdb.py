#!/usr/bin/env python3
# rdb.py -- an Amiga hard disk image (RDB) holding AMIX partitions.
#
#   rdb.py out.hdf NAME:DOSTYPE:MB[:IMAGE] ...
#
# Partitions follow in order from cylinder 2, after the RDB; AMIX numbers
# them s1, s2, ...  DOSTYPE: four characters, \N for a byte (UNI\1 root,
# resv swap, UNI\0 boot).  IMAGE fills the partition from its start;
# @DIR instead formats it FFS (DOS\1) holding DIR's files.  DOS partitions
# are bootable, the others are left unmounted by AmigaDOS.
# Geometry: 32 sectors x 16 heads, 256 KB cylinders.
import os, struct, sys

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

def ffs(top, nblocks, name):
	disk = bytearray(nblocks * BS)
	disk[0:4] = b'DOS\x01'
	root = (nblocks + 1) // 2
	nbm = (nblocks - 2 + 127 * 32 - 1) // (127 * 32)
	if nbm > 25:
		sys.exit('rdb.py: %s too big for the root bitmap' % top)
	used = set(range(2)) | set(range(root, root + nbm + 1))
	nxt = [root + nbm + 1]

	def alloc():
		b = nxt[0]
		if b >= nblocks:
			sys.exit('rdb.py: %s does not fit %d blocks' % (top, nblocks))
		nxt[0] += 1
		used.add(b)
		return b

	def put(b, blk, sec):
		if sec:
			struct.pack_into('>I', blk, 20, 0)
			struct.pack_into('>I', blk, 20, -sum(struct.unpack('>128I', blk)) & 0xffffffff)
		disk[b * BS:(b + 1) * BS] = blk

	def hdr(b, parent, nm, sec):
		blk = bytearray(BS)
		struct.pack_into('>II', blk, 0, 2, b)
		blk[432] = len(nm)
		blk[433:433 + len(nm)] = nm.encode()
		struct.pack_into('>iii', blk, 500, parent, 0, sec)
		return blk

	def hashname(nm):
		h = len(nm)
		for c in nm.upper().encode():
			h = (h * 13 + c) & 0x7ff
		return h % 72

	def link(dirblk, b, blk, nm):
		slot = 24 + 4 * hashname(nm)
		struct.pack_into('>I', blk, 496, struct.unpack_from('>I', dirblk, slot)[0])
		struct.pack_into('>I', dirblk, slot, b)

	def fill(path, dirb, dirblk):
		for nm in sorted(os.listdir(path)):
			full = os.path.join(path, nm)
			b = alloc()
			if os.path.isdir(full):
				blk = hdr(b, dirb, nm, 2)
				fill(full, b, blk)
			else:
				data = open(full, 'rb').read()
				blk = hdr(b, dirb, nm, -3)
				struct.pack_into('>I', blk, 324, len(data))
				ptrs = []
				for i in range(0, len(data), BS):
					d = alloc()
					disk[d * BS:d * BS + len(data[i:i + BS])] = data[i:i + BS]
					ptrs.append(d)
				cur, curb, chunk = blk, b, ptrs[:72]
				rest = ptrs[72:]
				while True:
					for i, d in enumerate(chunk):
						struct.pack_into('>I', cur, 24 + (71 - i) * 4, d)
					struct.pack_into('>I', cur, 8, len(chunk))
					if cur is blk and ptrs:
						struct.pack_into('>I', blk, 16, ptrs[0])
					if not rest:
						break
					e = alloc()
					struct.pack_into('>I', cur, 504, e)
					if cur is not blk:
						put(curb, cur, 1)
					cur, curb = bytearray(BS), e
					struct.pack_into('>II', cur, 0, 16, e)
					struct.pack_into('>iii', cur, 500, b, 0, -3)
					chunk, rest = rest[:72], rest[72:]
				if cur is not blk:
					put(curb, cur, 1)
			link(dirblk, b, blk, nm)
			put(b, blk, 1)

	rblk = bytearray(BS)
	struct.pack_into('>IIII', rblk, 0, 2, 0, 0, 72)
	struct.pack_into('>i', rblk, 312, -1)
	for i in range(nbm):
		struct.pack_into('>I', rblk, 316 + 4 * i, root + 1 + i)
	rblk[432] = len(name)
	rblk[433:433 + len(name)] = name.encode()
	struct.pack_into('>i', rblk, 508, 1)
	fill(top, root, rblk)
	put(root, rblk, 1)
	for i in range(nbm):
		bm = bytearray(BS)
		for n in range(127 * 32):
			b = 2 + i * 127 * 32 + n
			if b < nblocks and b not in used:
				w, bit = divmod(n, 32)
				at = 4 + 4 * w
				struct.pack_into('>I', bm, at, struct.unpack_from('>I', bm, at)[0] | 1 << bit)
		struct.pack_into('>I', bm, 0, -sum(struct.unpack('>128I', bm)) & 0xffffffff)
		disk[(root + 1 + i) * BS:(root + 2 + i) * BS] = bm
	return bytes(disk)

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
		dos = dt >> 8 == 0x444f53
		struct.pack_into('>4sIIIiI', p, 0, b'PART', 64, 0, 7, nxt, 1 if dos else 2)
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
			if img.startswith('@'):
				d = ffs(img[1:], (hi - lo + 1) * CYL, name)
			else:
				d = open(img, 'rb').read()
			if len(d) > (hi - lo + 1) * CYL * BS:
				sys.exit('rdb.py: %s does not fit %s' % (img, name))
			o.seek(lo * CYL * BS)
			o.write(d)

main()
