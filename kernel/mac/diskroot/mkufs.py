#!/usr/bin/env python3
# mkufs.py -- build an SVR4 ufs image (big-endian, m68k) from a manifest.
#
#   mkufs.py [-s MB] [-i IPG] [-t TIME] [-r SRCDIR] [-m MOUNTPT] manifest out.img
#
# Geometry as "mkfs -F ufs special SIZE 32 16 8192 1024 16 10 60": 32 sectors
# x 16 tracks per cylinder, 8 KB blocks, 1 KB fragments, 16 cylinders (4 MB)
# per group, 10% free, no rotational delay.  -s: size in MB, a multiple of 4.
# -i: inodes per group (default: one per 4 KB of data, as mkfs picks).
# The manifest format is fstree.py's.  Output is a function of the inputs only.
import struct, sys
from fstree import tree, depth_order, die, IFMT, IFIFO, IFCHR, IFDIR, IFBLK, IFREG, IFLNK

FS_MAGIC, CG_MAGIC, FSOKAY, EFT_MAGIC = 0x011954, 0x090255, 0x7c269d38, 0x90909090
SBOFF, SBSIZE, DEV_BSIZE, DIRBLKSIZ = 8192, 8192, 512, 512
NDADDR, NIADDR, NRPOS, MAXCPG, MAXIPG, MAXFRAG = 12, 3, 8, 32, 2048, 8
ROOTINO = 2

# struct fs, struct cg and struct dinode offsets, from the system headers
FS_OFF = dict(sblkno=8, cblkno=12, iblkno=16, dblkno=20, cgoffset=24, cgmask=28,
	time=32, size=36, dsize=40, ncg=44, bsize=48, fsize=52, frag=56, minfree=60,
	rotdelay=64, rps=68, bmask=72, fmask=76, bshift=80, fshift=84, maxcontig=88,
	maxbpg=92, fragshift=96, fsbtodb=100, sbsize=104, csmask=108, csshift=112,
	nindir=116, inopb=120, nspf=124, optim=128, state=132, csaddr=152, cssize=156,
	cgsize=160, ntrak=164, nsect=168, spc=172, ncyl=176, cpg=180, ipg=184, fpg=188,
	cstotal=192, fsmnt=212, cpc=856, postbl=860, magic=1372, rotbl=1376)
FS_SIZE, CG_SIZE, DINODE, CSUM = 1380, 988, 128, 16

def log2(n):
	return n.bit_length() - 1

class Ufs:
	def __init__(s, mb, ipg, now):
		s.now = now
		s.nsect, s.ntrak, s.bsize, s.fsize, s.cpg = 32, 16, 8192, 1024, 16
		s.frag = s.bsize // s.fsize
		s.nspf = s.fsize // DEV_BSIZE
		s.spc = s.nsect * s.ntrak
		s.fpg = s.cpg * s.spc // s.nspf
		s.size = mb * 1024 * 1024 // s.fsize		# fragments
		if s.size % s.fpg:
			die('size must be a multiple of %d MB' % (s.fpg * s.fsize >> 20))
		s.ncg = s.size // s.fpg
		s.ncyl = s.ncg * s.cpg
		s.inopb = s.bsize // DINODE
		s.nindir = s.bsize // 4
		s.sblkno = -(-(SBOFF + SBSIZE) // (s.fsize * s.frag)) * s.frag
		s.cblkno = s.sblkno + SBSIZE // s.fsize
		s.iblkno = s.cblkno + s.frag
		s.cgoffset = -(-(s.nsect // s.nspf) // s.frag) * s.frag
		s.cgmask = -1 << log2(s.ntrak)
		if ipg is None:
			ipg = (s.fpg - s.iblkno) * s.fsize // (4096 + DINODE) // s.inopb * s.inopb
		if ipg % s.inopb or not 0 < ipg <= MAXIPG:
			die('inodes per group must be a multiple of %d up to %d' % (s.inopb, MAXIPG))
		s.ipg = ipg
		s.dblkno = s.iblkno + ipg // (s.inopb // s.frag)
		s.cssize = -(-s.ncg * CSUM // s.fsize) * s.fsize
		s.csaddr = s.dblkno
		s.cpc = s.frag * s.nspf			# cylinders per rotational cycle
		sp = s.spc
		while s.cpc > 1 and sp % 2 == 0:
			s.cpc //= 2
			sp //= 2
		s.rotblsize = s.cpc * s.spc // (s.frag * s.nspf)
		s.sbsize = -(-(FS_SIZE + s.rotblsize) // s.fsize) * s.fsize
		s.cgsize = -(-(CG_SIZE + s.fpg // 8) // s.fsize) * s.fsize
		if s.sbsize > SBSIZE or s.cgsize > s.bsize or s.cpg > MAXCPG:
			die('geometry does not fit')
		s.img = bytearray(s.size * s.fsize)
		s.free = bytearray(s.size)		# 1 = free fragment
		s.dsize = 0
		for c in range(s.ncg):
			cbase, dmax = s.fpg * c, s.fpg * (c + 1)
			lo, hi = s.cgstart(c) + s.sblkno, s.cgstart(c) + s.dblkno
			if c == 0:
				hi += s.cssize // s.fsize
			else:
				s.free[cbase:lo] = b'\1' * (lo - cbase)
			s.free[hi:dmax] = b'\1' * (dmax - hi)
			s.dsize += (lo - cbase if c else 0) + dmax - hi
		s.nextblk = 0
		s.part, s.partk = None, 0
		s.inodes = {}			# ino -> (mode, uid, gid, nlink, size, db, ib, blocks, mtime)

	def cgstart(s, c):
		return s.fpg * c + s.cgoffset * (c & ~s.cgmask)

	def cbtocylno(s, d):
		return d * s.nspf // s.spc

	def cbtorpos(s, d):
		return d * s.nspf % s.spc % s.nsect * NRPOS // s.nsect

	# ---------------------------------------------------------- allocation

	def blkfree(s, f):
		return s.free[f:f + s.frag] == b'\1' * s.frag

	def allocblk(s):
		f = s.nextblk
		while f < s.size and not s.blkfree(f):
			f += s.frag
		if f >= s.size:
			die('out of space')
		s.free[f:f + s.frag] = bytes(s.frag)
		s.nextblk = f + s.frag
		return f

	def allocfrags(s, n):
		# tails share a partly used block; a new one is taken when they do not fit
		if s.part is not None:
			k = s.partk
			if k + n <= s.frag and s.free[s.part + k:s.part + k + n] == b'\1' * n:
				s.free[s.part + k:s.part + k + n] = bytes(n)
				s.partk += n
				return s.part + k
		f = s.allocblk()
		s.free[f + n:f + s.frag] = b'\1' * (s.frag - n)
		s.part, s.partk = f, n
		return f

	def put(s, f, data):
		s.img[f * s.fsize:f * s.fsize + len(data)] = data

	def layout(s, data, size):
		# returns db[], ib[], fragments held
		nb = -(-size // s.bsize)
		db, ib, held = [0] * NDADDR, [0] * NIADDR, 0
		addrs = []
		for lbn in range(nb):
			chunk = data[lbn * s.bsize:(lbn + 1) * s.bsize]
			if lbn == nb - 1 and nb <= NDADDR and size % s.bsize:
				n = -(-(size % s.bsize) // s.fsize)
				f = s.allocfrags(n)
			else:
				n, f = s.frag, s.allocblk()
			s.put(f, chunk)
			held += n
			if lbn < NDADDR:
				db[lbn] = f
			else:
				if lbn == NDADDR:
					ib[0] = s.allocblk()
					held += s.frag
				addrs.append(f)
		rest = addrs
		if rest:
			s.put(ib[0], struct.pack('>%dI' % min(len(rest), s.nindir), *rest[:s.nindir]))
			rest = rest[s.nindir:]
		if rest:
			ib[1] = s.allocblk()
			held += s.frag
			l1 = []
			while rest:
				b = s.allocblk()
				held += s.frag
				s.put(b, struct.pack('>%dI' % min(len(rest), s.nindir), *rest[:s.nindir]))
				rest = rest[s.nindir:]
				l1.append(b)
			if len(l1) > s.nindir:
				die('file too large')
			s.put(ib[1], struct.pack('>%dI' % len(l1), *l1))
		return db, ib, held

	# ---------------------------------------------------------- tree

	def dirdata(s, ents):
		out, chunk = bytearray(), bytearray()
		recs = []
		for nm, ino in ents:
			b = nm.encode('latin1')
			if len(b) > 255:
				die('name too long: ' + nm)
			recs.append((ino, b, 8 + ((len(b) + 1 + 3) & ~3)))
		i = 0
		while i < len(recs):
			used, blk = 0, []
			while i < len(recs) and used + recs[i][2] <= DIRBLKSIZ:
				blk.append(recs[i])
				used += recs[i][2]
				i += 1
			c = bytearray()
			for k, (ino, b, ln) in enumerate(blk):
				if k == len(blk) - 1:
					ln += DIRBLKSIZ - used
				c += struct.pack('>IHH', ino, ln, len(b)) + b.ljust(ln - 8, b'\0')
			out += c
		return bytes(out)

	def build(s, t, mntpt):
		if '/lost+found' not in t:
			from fstree import Ent
			t['/lost+found'] = Ent('d', 0o755)
		# lost+found gets inode 3, as mkfs gives it
		order = [p for p in depth_order(t) if p != '/lost+found']
		order.insert(1, '/lost+found')
		ino, nxt = {}, ROOTINO
		for p in order:
			if t[p].t != 'h':
				ino[p] = nxt
				nxt += 1
		if nxt > s.ncg * s.ipg:
			die('out of inodes (%d)' % (s.ncg * s.ipg))
		for p in order:
			if t[p].t == 'h':
				ino[p] = ino[t[p].data]
		kids = {}
		for p in order:
			if p != '/':
				kids.setdefault(p.rsplit('/', 1)[0] or '/', []).append(p)
		nlink = {}
		for p in order:
			nlink[ino[p]] = nlink.get(ino[p], 0) + (p != '/')
		for p in order:
			if t[p].t == 'd':
				nlink[ino[p]] += 1 + sum(1 for k in kids.get(p, []) if t[k].t == 'd')
		nlink[ROOTINO] += 1			# its own ".."
		s.ndir = [0] * s.ncg
		for p in sorted((p for p in order if t[p].t != 'h'), key=lambda p: ino[p]):
			e, i = t[p], ino[p]
			mt = s.now if e.mtime is None else e.mtime
			typ = {'d': IFDIR, 'f': IFREG, 'p': IFIFO, 'c': IFCHR, 'b': IFBLK, 'l': IFLNK}[e.t]
			db, ib, held, size = [0] * NDADDR, [0] * NIADDR, 0, 0
			if e.t == 'd':
				parent = ino[p.rsplit('/', 1)[0] or '/']
				ents = [('.', i), ('..', parent)]
				ents += [(k.rsplit('/', 1)[1], ino[k]) for k in sorted(kids.get(p, []))]
				data = s.dirdata(ents)
				if p == '/lost+found':
					data += b''.join(struct.pack('>IHH', 0, DIRBLKSIZ, 0).ljust(DIRBLKSIZ, b'\0')
						for _ in range((s.bsize - len(data) % s.bsize) % s.bsize // DIRBLKSIZ))
				s.ndir[i // s.ipg] += 1
			elif e.t in 'fl':
				data = e.data
			else:
				data = b''
			size = len(data)
			if size:
				db, ib, held = s.layout(data, size)
			if e.t in 'cb':
				maj, mn = e.dev
				db[1] = maj << 18 | mn
			s.inodes[i] = (typ | e.mode, e.uid, e.gid, nlink[i], size, db, ib,
				held * s.nspf, mt)
		s.mntpt = mntpt

	# ---------------------------------------------------------- metadata

	def finish(s):
		inopf = s.inopb // s.frag
		for i, (mode, uid, gid, nl, size, db, ib, blocks, mt) in s.inodes.items():
			c = i // s.ipg
			f = s.cgstart(c) + s.iblkno + (i % s.ipg) // inopf
			o = f * s.fsize + (i % inopf) * DINODE
			rec = struct.pack('>HhHHii', mode & 0xffff, nl, min(uid, 0xffff), min(gid, 0xffff),
				0, size)
			rec += struct.pack('>6i', mt, 0, mt, 0, mt, 0)
			rec += struct.pack('>12i3i', *(db + ib))
			rec += struct.pack('>iiiIIII', 0, blocks, 0, mode, uid, gid, EFT_MAGIC)
			s.img[o:o + DINODE] = rec
		tot = [0, 0, 0, 0]
		csum = bytearray()
		for c in range(s.ncg):
			cbase = s.fpg * c
			cg = bytearray(s.cgsize)
			nbfree = nffree = 0
			frsum, btot = [0] * MAXFRAG, [0] * MAXCPG
			b = [[0] * NRPOS for _ in range(MAXCPG)]
			for d in range(0, s.fpg, s.frag):
				bits = s.free[cbase + d:cbase + d + s.frag]
				if bits == b'\1' * s.frag:
					nbfree += 1
					btot[s.cbtocylno(d)] += 1
					b[s.cbtocylno(d)][s.cbtorpos(d)] += 1
					continue
				run = 0
				for x in bits + b'\0':
					if x:
						run += 1
						nffree += 1
					elif run:
						frsum[run] += 1
						run = 0
			used = [i for i in s.inodes if i // s.ipg == c]
			nifree = s.ipg - len(used) - (ROOTINO if c == 0 else 0)
			cs = (s.ndir[c], nbfree, nifree, nffree)
			tot = [x + y for x, y in zip(tot, cs)]
			csum += struct.pack('>4i', *cs)
			# the last group's count is ncyl % cpg, 0 when it is full, as mkfs writes it
			ncyl = s.cpg if c < s.ncg - 1 else s.ncyl % s.cpg
			struct.pack_into('>iihhi4i', cg, 8, s.now, c, ncyl, s.ipg, s.fpg, *cs)
			struct.pack_into('>8i', cg, 52, *frsum)
			struct.pack_into('>32i', cg, 84, *btot)
			struct.pack_into('>256h', cg, 212, *[x for row in b for x in row])
			iused = bytearray(MAXIPG // 8)
			for i in used + ([0, 1] if c == 0 else []):
				iused[(i % s.ipg) // 8] |= 1 << (i % 8)
			cg[724:724 + len(iused)] = iused
			struct.pack_into('>i', cg, 980, CG_MAGIC)
			fmap = bytearray(s.fpg // 8)
			for d in range(s.fpg):
				if s.free[cbase + d]:
					fmap[d // 8] |= 1 << (d % 8)
			cg[984:984 + len(fmap)] = fmap
			s.put(s.cgstart(c) + s.cblkno, cg)
		s.put(s.csaddr, csum)
		sb = bytearray(s.sbsize)
		v = dict(sblkno=s.sblkno, cblkno=s.cblkno, iblkno=s.iblkno, dblkno=s.dblkno,
			cgoffset=s.cgoffset, cgmask=s.cgmask, time=s.now, size=s.size, dsize=s.dsize,
			ncg=s.ncg, bsize=s.bsize, fsize=s.fsize, frag=s.frag, minfree=10, rotdelay=0,
			rps=60, bmask=-s.bsize, fmask=-s.fsize, bshift=log2(s.bsize),
			fshift=log2(s.fsize), maxcontig=1, maxbpg=s.fpg // s.frag // 2,
			fragshift=log2(s.frag), fsbtodb=log2(s.nspf), sbsize=s.sbsize,
			csmask=-(s.bsize // CSUM), csshift=log2(s.bsize // CSUM), nindir=s.nindir,
			inopb=s.inopb, nspf=s.nspf, optim=0, state=(FSOKAY - s.now) & 0xffffffff,
			csaddr=s.csaddr, cssize=s.cssize, cgsize=s.cgsize, ntrak=s.ntrak,
			nsect=s.nsect, spc=s.spc, ncyl=s.ncyl, cpg=s.cpg, ipg=s.ipg, fpg=s.fpg,
			cpc=s.cpc, magic=FS_MAGIC)
		for k, x in v.items():
			struct.pack_into('>I', sb, FS_OFF[k], x & 0xffffffff)
		struct.pack_into('>4i', sb, FS_OFF['cstotal'], *tot)
		sb[FS_OFF['fsmnt']:FS_OFF['fsmnt'] + len(s.mntpt)] = s.mntpt.encode()
		# rotational layout: blocks at each (cylinder, position), chained by rotbl
		postbl = [[-1] * NRPOS for _ in range(MAXCPG)]
		rotbl = [0] * s.rotblsize
		for f in range((s.rotblsize - 1) * s.frag, -1, -s.frag):
			cyl, rpos, blk = s.cbtocylno(f), s.cbtorpos(f), f // s.frag
			rotbl[blk] = 0 if postbl[cyl][rpos] == -1 else postbl[cyl][rpos] - blk
			postbl[cyl][rpos] = blk
		struct.pack_into('>256h', sb, FS_OFF['postbl'], *[x for row in postbl for x in row])
		sb[FS_OFF['rotbl']:FS_OFF['rotbl'] + s.rotblsize] = bytes(rotbl)
		s.img[SBOFF:SBOFF + s.sbsize] = sb
		for c in range(s.ncg):
			s.put(s.cgstart(c) + s.sblkno, sb)
		s.tot = tot

def main():
	a = sys.argv[1:]
	mb, ipg, now, src, mnt = 64, None, 0x2B000000, '.', '/'
	while a and a[0].startswith('-'):
		o, v = a[0], a[1]
		a = a[2:]
		if o == '-s': mb = int(v)
		elif o == '-i': ipg = int(v)
		elif o == '-t': now = int(v)
		elif o == '-r': src = v
		elif o == '-m': mnt = v
		else: die('unknown option ' + o)
	if len(a) != 2:
		die('usage: mkufs.py [-s MB] [-i IPG] [-t TIME] [-r SRCDIR] [-m MOUNTPT] manifest out.img')
	fs = Ufs(mb, ipg, now)
	fs.build(tree(a[0], src), mnt)
	fs.finish()
	with open(a[1], 'wb') as f:
		f.write(fs.img)
	ndir, nbfree, nifree, nffree = fs.tot
	print('mkufs: %s: %d MB, %d groups, %d inodes (%d free), %d blocks + %d frags free, %d dirs'
		% (a[1], mb, fs.ncg, fs.ncg * fs.ipg, nifree, nbfree, nffree, ndir))

main()
