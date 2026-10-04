#!/usr/bin/env python3
# mks5fs.py -- build an SVR4 s5 filesystem image (big-endian, m68k) from a manifest.
#
#   mks5fs.py [-s KB] [-i NINODE] [-b 512|1024|2048] [-t TIME] [-r SRCDIR]
#             [-n FNAME] [-p FPACK] manifest out.img
#
# The manifest format is fstree.py's.  Output is a function of the inputs only.
import struct, sys, os
from fstree import tree, depth_order, die, IFMT, IFIFO, IFCHR, IFDIR, IFBLK, IFREG, IFLNK

FSMAGIC, FSOKAY = 0xfd187e20, 0x7c269d38
NICFREE, NICINOD, NADDR, NDIRECT = 50, 100, 13, 10
ROOTINO = 2

# ------------------------------------------------------------------ image

class Ino:
	def __init__(s, mode, uid=0, gid=0, mtime=0):
		s.mode, s.uid, s.gid, s.nlink, s.mtime = mode, uid, gid, 0, mtime
		s.data, s.addr, s.ents = b'', [0] * NADDR, None

class Fs:
	def __init__(s, bsize, nblk, ninode, now):
		s.bsize, s.nblk, s.now = bsize, nblk, now
		s.inopb = bsize // 64
		s.ninode = (ninode + s.inopb - 1) // s.inopb * s.inopb
		if s.ninode > 65500:
			die('too many inodes (%d)' % s.ninode)
		s.isize = 2 + s.ninode // s.inopb	# first data block
		s.nind = bsize // 4
		s.img = bytearray(nblk * bsize)
		s.next = s.isize
		s.inos = {}
		s.paths = {}
		s.inos[1] = Ino(IFREG)		# bad-block inode, never allocated
		root = s.newino(IFDIR | 0o755)
		assert root == ROOTINO
		s.paths['/'] = root
		s.inos[root].ents = [('.', root), ('..', root)]
		s.inos[root].nlink = 2

	def newino(s, mode, uid=0, gid=0, mtime=None):
		n = len(s.inos) + 1
		if n > s.ninode:
			die('out of inodes (%d)' % s.ninode)
		s.inos[n] = Ino(mode, uid, gid, s.now if mtime is None else mtime)
		return n

	def enter(s, path, ino):
		d, name = os.path.split(path)
		if len(name.encode('latin1')) > 14:
			die('name too long: ' + path)
		s.inos[s.paths[d]].ents.append((name, ino))
		s.inos[ino].nlink += 1
		s.paths[path] = ino

	def add(s, path, e):
		if path == '/':
			n = s.inos[ROOTINO]
			n.mode, n.uid, n.gid = IFDIR | e.mode, e.uid, e.gid
			return
		if e.t == 'h':
			s.enter(path, s.paths[e.data])
			return
		typ = {'d': IFDIR, 'f': IFREG, 'p': IFIFO, 'c': IFCHR, 'b': IFBLK, 'l': IFLNK}[e.t]
		i = s.newino(typ | e.mode, e.uid, e.gid, e.mtime)
		n = s.inos[i]
		s.enter(path, i)
		if e.t == 'd':
			n.ents = [('.', i), ('..', s.paths[os.path.dirname(path)])]
			n.nlink += 1
			s.inos[n.ents[1][1]].nlink += 1
		elif e.t in 'fl':
			n.data = e.data
		elif e.t in 'cb':
			maj, mn = e.dev
			old = (maj << 8 | mn) if maj < 128 and mn < 256 else 0xffffff
			n.addr[0:4] = [old, 1, maj & 0xff, mn & 0x3ffff]

	def alloc(s):
		if s.next >= s.nblk:
			die('out of blocks (%d)' % s.nblk)
		s.next += 1
		return s.next - 1

	def putblk(s, b, data):
		s.img[b * s.bsize:b * s.bsize + len(data)] = data

	def indir(s, blocks, level):
		# returns block number of an indirect tree over `blocks`, consuming them
		b = s.alloc()
		ptrs = []
		while blocks and len(ptrs) < s.nind:
			if level == 1:
				ptrs.append(blocks.pop(0))
			else:
				ptrs.append(s.indir(blocks, level - 1))
		s.putblk(b, struct.pack('>%dI' % len(ptrs), *ptrs))
		return b

	def layout(s, n):
		if n.ents is not None:
			n.data = b''.join(struct.pack('>H14s', i, nm.encode('latin1')) for nm, i in n.ents)
		if (n.mode & IFMT) in (IFCHR, IFBLK, IFIFO):
			return
		nb = (len(n.data) + s.bsize - 1) // s.bsize
		blocks = []
		for k in range(nb):
			b = s.alloc()
			s.putblk(b, n.data[k * s.bsize:(k + 1) * s.bsize])
			blocks.append(b)
		n.addr[:NDIRECT] = (blocks[:NDIRECT] + [0] * NDIRECT)[:NDIRECT]
		rest = blocks[NDIRECT:]
		for lvl in (1, 2, 3):
			if rest:
				n.addr[NDIRECT + lvl - 1] = s.indir(rest, lvl)
		if rest:
			die('file too large')

	def finish(s, fname, fpack):
		for i in sorted(s.inos):
			s.layout(s.inos[i])
		for i in range(1, s.ninode + 1):
			n = s.inos.get(i)
			if n is None:
				continue
			a = b''.join(struct.pack('>I', x)[1:] for x in n.addr)
			rec = struct.pack('>HHHHI39sBIII', n.mode, n.nlink, n.uid, n.gid,
				len(n.data) if (n.mode & IFMT) not in (IFCHR, IFBLK, IFIFO) else 0,
				a, 0, n.mtime, n.mtime, n.mtime)
			o = (2 + (i - 1) // s.inopb) * s.bsize + (i - 1) % s.inopb * 64
			s.img[o:o + 64] = rec
		# free list, highest block first, so low blocks are handed out first
		nfree, free = 0, [0] * NICFREE
		def bfree(b):
			nonlocal nfree
			if nfree >= NICFREE:
				s.putblk(b, struct.pack('>i%di' % NICFREE, nfree, *free))
				nfree = 0
			free[nfree] = b
			nfree += 1
		bfree(0)
		tfree = 0
		for b in range(s.nblk - 1, s.next - 1, -1):
			bfree(b)
			tfree += 1
		freeino = [i for i in range(ROOTINO + 1, s.ninode + 1) if i not in s.inos]
		ilist = freeino[:NICINOD]
		sb = bytearray(512)
		struct.pack_into('>H', sb, 0, s.isize)
		struct.pack_into('>i', sb, 4, s.nblk)
		struct.pack_into('>h', sb, 8, nfree)
		struct.pack_into('>%di' % NICFREE, sb, 12, *free)
		struct.pack_into('>h', sb, 212, len(ilist))
		struct.pack_into('>%dH' % len(ilist), sb, 214, *ilist)
		struct.pack_into('>I', sb, 420, s.now)
		struct.pack_into('>iH6s6s', sb, 432, tfree, len(freeino), fname, fpack)
		struct.pack_into('>III', sb, 500, (FSOKAY - s.now) & 0xffffffff, FSMAGIC,
			{512: 1, 1024: 2, 2048: 3}[s.bsize])
		s.img[512:1024] = sb

def main():
	a = sys.argv[1:]
	kb, nino, bsize, now, src = 2048, 256, 1024, 0x2B000000, '.'
	fname, fpack = b'root', b'disk'
	while a and a[0].startswith('-'):
		o, v = a[0], a[1]
		a = a[2:]
		if o == '-s': kb = int(v)
		elif o == '-i': nino = int(v)
		elif o == '-b': bsize = int(v)
		elif o == '-t': now = int(v)
		elif o == '-r': src = v
		elif o == '-n': fname = v.encode()[:6]
		elif o == '-p': fpack = v.encode()[:6]
		else: die('unknown option ' + o)
	if len(a) != 2:
		die('usage: mks5fs.py [-s KB] [-i N] [-b BSIZE] [-t TIME] [-r SRCDIR] '
			'[-n FNAME] [-p FPACK] manifest out.img')
	t = tree(a[0], src)
	fs = Fs(bsize, kb * 1024 // bsize, nino, now)
	for p in depth_order(t):
		fs.add(p, t[p])
	fs.finish(fname, fpack)
	with open(a[1], 'wb') as f:
		f.write(fs.img)
	print('mks5fs: %s: %d x %d-byte blocks, %d inodes, %d used, %d data blocks used'
		% (a[1], fs.nblk, bsize, fs.ninode, len(fs.inos), fs.next - fs.isize))

main()
