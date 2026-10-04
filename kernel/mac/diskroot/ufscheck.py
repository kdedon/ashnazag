#!/usr/bin/env python3
# ufscheck.py -- read a ufs image back: list it and check its consistency.
#
#   ufscheck.py image [-l] [-x DIR]
#
#   -l      list every file (ls -l style with inode, device, link target)
#   -x DIR  extract regular files and symlink targets under DIR (for comparison)
#
# Checks: superblock magic and clean state, directory format, inode block
# lists (fragments only at the end of short files), every fragment used once,
# link counts, cylinder group maps, summaries and totals.  Exit 1 on any error.
import struct, sys, os

FS_MAGIC, CG_MAGIC, FSOKAY, EFT_MAGIC = 0x011954, 0x090255, 0x7c269d38, 0x90909090
NDADDR, NRPOS, DIRBLKSIZ = 12, 8, 512

err = []
def bad(m):
	err.append(m)

a = sys.argv[1:]
img = open(a[0], 'rb').read()
listing = '-l' in a
xdir = a[a.index('-x') + 1] if '-x' in a else None

sb = img[8192:8192 + 1380]
i32 = lambda b, o: struct.unpack_from('>i', b, o)[0]
if i32(sb, 1372) != FS_MAGIC:
	sys.exit('ufscheck: bad magic 0x%x' % i32(sb, 1372))
F = {k: i32(sb, o) for k, o in dict(sblkno=8, cblkno=12, iblkno=16, dblkno=20, cgoffset=24,
	cgmask=28, time=32, size=36, dsize=40, ncg=44, bsize=48, fsize=52, frag=56, state=132,
	nspf=124, inopb=120, nindir=116, csaddr=152, cssize=156, ntrak=164, nsect=168, spc=172,
	ncyl=176, cpg=180, ipg=184, fpg=188).items()}
bsize, fsize, frag, ipg, fpg, ncg = F['bsize'], F['fsize'], F['frag'], F['ipg'], F['fpg'], F['ncg']
if (F['state'] + F['time']) & 0xffffffff != FSOKAY:
	bad('fs_state not FSOKAY for fs_time')
if F['size'] * fsize != len(img):
	bad('fs_size %d fragments != image %d bytes' % (F['size'], len(img)))
print('ufscheck: %s: bsize %d fsize %d size %d ncg %d ipg %d fpg %d'
	% (a[0], bsize, fsize, F['size'], ncg, ipg, fpg))

def cgstart(c):
	return fpg * c + F['cgoffset'] * (c & ~F['cgmask'])

# data area: everything a group does not hold metadata in
data = bytearray(F['size'])
for c in range(ncg):
	cb, lo, hi = fpg * c, cgstart(c) + F['sblkno'], cgstart(c) + F['dblkno']
	if c == 0:
		hi += -(-F['cssize'] // fsize)
	else:
		data[cb:lo] = b'\1' * (lo - cb)
	data[hi:min(fpg * (c + 1), F['size'])] = b'\1' * (min(fpg * (c + 1), F['size']) - hi)

owner = {}
def claim(f, n, who):
	if f % frag + n > frag:
		bad('%s: fragments %d+%d cross a block' % (who, f, n))
	for x in range(f, f + n):
		if not 0 <= x < F['size'] or not data[x]:
			bad('%s: fragment %d outside the data area' % (who, x))
		elif x in owner:
			bad('fragment %d used by %s and %s' % (x, owner[x], who))
		else:
			owner[x] = who

def frg(f):
	return img[f * fsize:(f + 1) * fsize]

def dinode(i):
	c = i // ipg
	f = cgstart(c) + F['iblkno'] + (i % ipg) // (F['inopb'] // frag)
	o = f * fsize + (i % (F['inopb'] // frag)) * 128
	d = img[o:o + 128]
	sm, nl = struct.unpack_from('>Hh', d, 0)
	hi, size = struct.unpack_from('>ii', d, 8)
	db = list(struct.unpack_from('>12i', d, 40))
	ib = list(struct.unpack_from('>3i', d, 88))
	blocks, mode, uid, gid, eft = struct.unpack_from('>iIIII', d, 104)
	if eft != EFT_MAGIC:
		mode, uid, gid = sm, struct.unpack_from('>H', d, 4)[0], struct.unpack_from('>H', d, 6)[0]
	return dict(mode=mode, smode=sm, nlink=nl, size=size, hi=hi, db=db, ib=ib,
		blocks=blocks, uid=uid, gid=gid, mtime=struct.unpack_from('>i', d, 24)[0])

def content(i, n):
	# data of inode i; claims its fragments and indirect blocks
	size, who = n['size'], 'ino %d' % i
	nb = -(-size // bsize)
	held = 0
	out = bytearray()
	ptrs = []
	for lbn in range(min(nb, NDADDR)):
		ptrs.append(n['db'][lbn])
	def indir(b, lvl):
		nonlocal held
		claim(b, frag, who + ' indirect')
		held += frag
		p = struct.unpack_from('>%di' % F['nindir'], img, b * fsize)
		for x in p:
			if len(ptrs) >= nb:
				return
			if lvl == 1:
				ptrs.append(x)
			else:
				indir(x, lvl - 1)
	for lvl in (1, 2, 3):
		if len(ptrs) < nb:
			indir(n['ib'][lvl - 1], lvl)
	for lbn, f in enumerate(ptrs):
		k = frag
		if lbn == nb - 1 and nb <= NDADDR and size % bsize:
			k = -(-(size % bsize) // fsize)
		if f == 0:
			bad('%s: hole at block %d' % (who, lbn))
			out += bytes(bsize)
			continue
		claim(f, k, who)
		held += k
		out += img[f * fsize:(f + k) * fsize]
	if held * F['nspf'] != n['blocks']:
		bad('%s: di_blocks %d, holds %d' % (who, n['blocks'], held * F['nspf']))
	return bytes(out[:size])

T = {0o010000: 'p', 0o020000: 'c', 0o040000: 'd', 0o060000: 'b', 0o100000: '-', 0o120000: 'l'}
refs, seen, nodes, ndir = {}, set(), {}, [0] * ncg
def visit(i, path, parent):
	n = dinode(i)
	nodes[i] = n
	refs[i] = refs.get(i, 0) + 1
	t = T.get(n['mode'] & 0o170000)
	if t is None:
		bad('%s: ino %d bad mode 0%o' % (path, i, n['mode']))
		return
	if n['smode'] != n['mode'] & 0xffff:
		bad('%s: 16-bit mode 0%o, mode 0%o' % (path, n['smode'], n['mode']))
	if i in seen:
		if t == 'd':
			bad('%s: directory ino %d linked twice' % (path, i))
		if listing:
			print('%5d %s%s link to earlier ino' % (i, t, path))
		return
	seen.add(i)
	extra = ''
	if t in 'cb':
		dev = n['db'][1]
		extra = ' %d,%d' % (dev >> 18, dev & 0x3ffff)
	elif t != 'p':
		d = content(i, n)
		if t == 'l':
			extra = ' -> ' + d.decode('latin1')
		if xdir and t in '-l':
			out = os.path.join(xdir, path.lstrip('/'))
			os.makedirs(os.path.dirname(out), exist_ok=True)
			open(out + ('.symlink' if t == 'l' else ''), 'wb').write(d)
	if listing:
		print('%5d %s%s %2d %3d %3d %7d %s%s' % (i, t, oct(n['mode'] & 0o7777)[2:].rjust(4, '0'),
			n['nlink'], n['uid'], n['gid'], n['size'], path, extra))
	if t != 'd':
		return
	ndir[i // ipg] += 1
	if n['size'] % DIRBLKSIZ:
		bad('%s: directory size %d' % (path, n['size']))
	names, kids = [], []
	for c0 in range(0, len(d), DIRBLKSIZ):
		o = c0
		while o < c0 + DIRBLKSIZ:
			ino, rl, nl = struct.unpack_from('>IHH', d, o)
			if rl < 8 + ((nl + 1 + 3) & ~3) or o + rl > c0 + DIRBLKSIZ or rl % 4:
				bad('%s: bad entry at %d' % (path, o))
				break
			nm = d[o + 8:o + 8 + nl].decode('latin1')
			if ino:
				names.append(nm)
				kids.append((ino, nm))
			o += rl
	if names[:2] != ['.', '..']:
		bad('%s: does not start with . and ..' % path)
	if len(set(names)) != len(names):
		bad('%s: duplicate names' % path)
	for ino, nm in kids:
		if not 0 < ino < ncg * ipg:
			bad('%s/%s: ino %d out of range' % (path, nm, ino))
		elif nm == '.':
			if ino != i: bad('%s: . is %d' % (path, ino))
			refs[i] += 1
		elif nm == '..':
			if ino != parent: bad('%s: .. is %d, want %d' % (path, ino, parent))
			refs[ino] = refs.get(ino, 0) + 1
		else:
			visit(ino, path.rstrip('/') + '/' + nm, i)

refs[2] = -1		# root's own entry comes from its '..'
visit(2, '/', 2)
for i, n in nodes.items():
	if n['nlink'] != refs[i]:
		bad('ino %d: nlink %d, %d references' % (i, n['nlink'], refs[i]))

tot = [0, 0, 0, 0]
csum = img[F['csaddr'] * fsize:F['csaddr'] * fsize + 16 * ncg]
for c in range(ncg):
	cb = fpg * c
	cg = frg(cgstart(c) + F['cblkno']) + frg(cgstart(c) + F['cblkno'] + 1)
	if i32(cg, 980) != CG_MAGIC or i32(cg, 12) != c:
		bad('cg %d: bad magic or index' % c)
		continue
	nbfree = nffree = nifree = 0
	frsum = [0] * 8
	for d in range(fpg):
		isfree = cg[984 + d // 8] >> (d % 8) & 1
		if isfree and (cb + d) in owner:
			bad('cg %d: fragment %d free and used' % (c, cb + d))
		if not isfree and data[cb + d] and (cb + d) not in owner:
			bad('cg %d: fragment %d neither used nor free' % (c, cb + d))
	for d in range(0, fpg, frag):
		bits = [cg[984 + (d + k) // 8] >> ((d + k) % 8) & 1 for k in range(frag)]
		if all(bits):
			nbfree += 1
			continue
		run = 0
		for x in bits + [0]:
			if x:
				run += 1
				nffree += 1
			elif run:
				frsum[run] += 1
				run = 0
	for k in range(ipg):
		used = cg[724 + k // 8] >> (k % 8) & 1
		i = c * ipg + k
		live = i in nodes or i < 2
		if used != live:
			bad('ino %d: %s in the map but %s' % (i, 'used' if used else 'free',
				'reachable' if live else 'unreachable'))
		nifree += not used
	cs = list(struct.unpack_from('>4i', cg, 24))
	want = [ndir[c], nbfree, nifree, nffree]
	if cs != want:
		bad('cg %d: summary %s, counted %s' % (c, cs, want))
	if list(struct.unpack_from('>8i', cg, 52))[1:] != frsum[1:]:
		bad('cg %d: frsum' % c)
	if list(struct.unpack_from('>4i', csum, 16 * c)) != cs:
		bad('cg %d: summary area differs from the group' % c)
	tot = [x + y for x, y in zip(tot, want)]
if list(struct.unpack_from('>4i', sb, 192)) != tot:
	bad('fs_cstotal %s, counted %s' % (list(struct.unpack_from('>4i', sb, 192)), tot))

print('ufscheck: %d files, %d dirs, %d blocks + %d frags free, %d free inodes'
	% (len(nodes), tot[0], tot[1], tot[3], tot[2]))
for m in err:
	print('ERROR: ' + m)
print('ufscheck: %s' % ('FAIL' if err else 'OK'))
sys.exit(1 if err else 0)
