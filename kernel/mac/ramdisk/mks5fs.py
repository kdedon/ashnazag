#!/usr/bin/env python3
# mks5fs.py -- build an SVR4 s5 filesystem image (big-endian, m68k) from a manifest.
#
#   mks5fs.py [-s KB] [-i NINODE] [-b 512|1024|2048] [-t TIME] [-r SRCDIR] manifest out.img
#
# Manifest lines (octal mode, '#' comments):
#   d PATH MODE UID GID              directory
#   f PATH MODE UID GID SRC          regular file, contents from SRCDIR/SRC
#   e PATH MODE UID GID              empty regular file
#   p PATH MODE UID GID              fifo
#   c|b PATH MODE UID GID MAJ MIN    device (EFT form, old 16-bit copy in addr[0])
#   l PATH TARGET                    symbolic link
#   h PATH EXISTING                  hard link
# Parents come before children.  Output is a function of the inputs only.
import struct, sys, os

FSMAGIC, FSOKAY = 0xfd187e20, 0x7c269d38
NICFREE, NICINOD, NADDR, NDIRECT = 50, 100, 13, 10
IFIFO, IFCHR, IFDIR, IFBLK, IFREG, IFLNK = 0o010000, 0o020000, 0o040000, 0o060000, 0o100000, 0o120000
ROOTINO = 2

class Ino:
	def __init__(s, mode, uid=0, gid=0):
		s.mode, s.uid, s.gid, s.nlink = mode, uid, gid, 0
		s.data, s.addr, s.ents = b'', [0] * NADDR, None

def die(msg):
	sys.exit('mks5fs: ' + msg)

class Fs:
	def __init__(s, bsize, nblk, ninode, now):
		s.bsize, s.nblk, s.now = bsize, nblk, now
		s.inopb = bsize // 64
		s.ninode = (ninode + s.inopb - 1) // s.inopb * s.inopb
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

	def newino(s, mode, uid=0, gid=0):
		n = len(s.inos) + 1
		if n > s.ninode:
			die('out of inodes (%d)' % s.ninode)
		s.inos[n] = Ino(mode, uid, gid)
		return n

	def enter(s, path, ino):
		d, name = os.path.split(path.rstrip('/'))
		if len(name.encode(errors='surrogateescape')) > 14:
			die('name too long: ' + path)
		if path in s.paths:
			die('duplicate: ' + path)
		p = s.paths.get(d or '/')
		if p is None or s.inos[p].ents is None:
			die('no parent directory for ' + path)
		s.inos[p].ents.append((name, ino))
		s.inos[ino].nlink += 1
		s.paths[path] = ino

	def add(s, f):
		t, path = f[0], f[1]
		if t == 'l':
			i = s.newino(IFLNK | 0o777)
			s.inos[i].data = f[2].encode(errors='surrogateescape')
			s.enter(path, i)
			return
		if t == 'h':
			if f[2] not in s.paths or s.inos[s.paths[f[2]]].ents is not None:
				die('bad hard link target: ' + f[2])
			s.enter(path, s.paths[f[2]])
			return
		mode, uid, gid = int(f[2], 8), int(f[3]), int(f[4])
		typ = {'d': IFDIR, 'f': IFREG, 'e': IFREG, 'p': IFIFO, 'c': IFCHR, 'b': IFBLK}[t]
		i = s.newino(typ | mode, uid, gid)
		n = s.inos[i]
		s.enter(path, i)
		if t == 'd':
			n.ents = [('.', i), ('..', s.paths[os.path.dirname(path) or '/'])]
			n.nlink += 1
			s.inos[n.ents[1][1]].nlink += 1
		elif t == 'f':
			n.data = open(os.path.join(s.src, f[5]), 'rb').read()
		elif t in 'cb':
			maj, mn = int(f[5]), int(f[6])
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
		span = s.nind ** (level - 1)
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
			n.data = b''.join(struct.pack('>H14s', i, nm.encode(errors='surrogateescape')) for nm, i in n.ents)
		if (n.mode & 0o170000) in (IFCHR, IFBLK, IFIFO):
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
				len(n.data) if (n.mode & 0o170000) not in (IFCHR, IFBLK, IFIFO) else 0,
				a, 0, s.now, s.now, s.now)
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
	while a and a[0].startswith('-'):
		o, v = a[0], a[1]
		a = a[2:]
		if o == '-s': kb = int(v)
		elif o == '-i': nino = int(v)
		elif o == '-b': bsize = int(v)
		elif o == '-t': now = int(v)
		elif o == '-r': src = v
		else: die('unknown option ' + o)
	if len(a) != 2:
		die('usage: mks5fs.py [-s KB] [-i N] [-b BSIZE] [-t TIME] [-r SRCDIR] manifest out.img')
	fs = Fs(bsize, kb * 1024 // bsize, nino, now)
	fs.src = src
	for ln in open(a[0], errors='surrogateescape'):	# names may be Mac Roman
		f = ln.split('#')[0].split()
		if f:
			fs.add(f)
	fs.finish(b'root', b'ramd')
	open(a[1], 'wb').write(fs.img)
	print('mks5fs: %s: %d x %d-byte blocks, %d inodes, %d used, %d data blocks used'
		% (a[1], fs.nblk, bsize, fs.ninode, len(fs.inos), fs.next - fs.isize))

main()
