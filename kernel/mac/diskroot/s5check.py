#!/usr/bin/env python3
# s5check.py -- read an s5 image back: list it and check its consistency.
#
#   s5check.py image [-l] [-x DIR]
#
#   -l      list every file (ls -l style with inode, device, link target)
#   -x DIR  extract regular files and symlink targets under DIR (for comparison)
#
# Checks: superblock magic/type/state, inode and directory structure, link
# counts, every block used exactly once (files, indirect blocks, free list),
# free counts, free-inode cache.  Exit 1 on any error.
import struct, sys, os

err = []
def bad(m):
	err.append(m)

a = sys.argv[1:]
img = open(a[0], 'rb').read()
listing = '-l' in a
xdir = a[a.index('-x') + 1] if '-x' in a else None

sb = img[512:1024]
u16 = lambda b, o: struct.unpack_from('>H', b, o)[0]
u32 = lambda b, o: struct.unpack_from('>I', b, o)[0]
magic, ftype, state, stime = u32(sb, 504), u32(sb, 508), u32(sb, 500), u32(sb, 420)
if magic != 0xfd187e20:
	sys.exit('s5check: bad magic 0x%08x' % magic)
bsize = {1: 512, 2: 1024, 3: 2048}.get(ftype)
if bsize is None:
	sys.exit('s5check: bad s_type %d' % ftype)
isize, fsize = u16(sb, 0), u32(sb, 4)
if (state + stime) & 0xffffffff != 0x7c269d38:
	bad('s_state not FsOKAY for s_time')
if fsize * bsize != len(img):
	bad('s_fsize %d blocks != image %d bytes' % (fsize, len(img)))
inopb = bsize // 64
ninode = (isize - 2) * inopb
nind = bsize // 4
print('s5check: %s: bsize %d fsize %d isize %d inodes %d name %r pack %r'
	% (a[0], bsize, fsize, isize, ninode, sb[438:444].rstrip(b'\0'), sb[444:450].rstrip(b'\0')))

def blk(b):
	return img[b * bsize:(b + 1) * bsize]

def dinode(i):
	o = (2 + (i - 1) // inopb) * bsize + (i - 1) % inopb * 64
	d = img[o:o + 64]
	mode, nlink, uid, gid, size = struct.unpack_from('>HHHHI', d, 0)
	addr = [int.from_bytes(d[12 + 3 * k:15 + 3 * k], 'big') for k in range(13)]
	return dict(mode=mode, nlink=nlink, uid=uid, gid=gid, size=size, addr=addr,
		mtime=u32(d, 56))

owner = {}
def claim(b, who):
	if not isize <= b < fsize:
		bad('%s: block %d out of range' % (who, b))
	elif b in owner:
		bad('block %d used by %s and %s' % (b, owner[b], who))
	else:
		owner[b] = who

def fileblocks(i, n):
	# data blocks of inode i in order; claims data and indirect blocks
	need = (n['size'] + bsize - 1) // bsize
	out = []
	def walk(b, lvl):
		claim(b, 'ino %d indirect' % i)
		for k in range(nind):
			if len(out) >= need:
				return
			p = u32(blk(b), 4 * k)
			if lvl == 1:
				out.append(p)
			else:
				walk(p, lvl - 1)
	for k in range(10):
		if len(out) < need:
			out.append(n['addr'][k])
	for lvl in (1, 2, 3):
		if len(out) < need:
			walk(n['addr'][9 + lvl], lvl)
	for b in out:
		claim(b, 'ino %d' % i)
	return out

def content(i, n):
	return b''.join(blk(b) for b in fileblocks(i, n))[:n['size']]

T = {0o010000: 'p', 0o020000: 'c', 0o040000: 'd', 0o060000: 'b', 0o100000: '-', 0o120000: 'l'}
refs, seen, nodes = {}, set(), {}
def visit(i, path, parent):
	n = dinode(i)
	nodes[i] = n
	refs[i] = refs.get(i, 0) + 1
	t = T.get(n['mode'] & 0o170000)
	if t is None:
		bad('%s: ino %d bad mode 0%o' % (path, i, n['mode']))
		return
	if i in seen:
		if t == 'd':
			bad('%s: directory ino %d linked twice' % (path, i))
		extra = ''
		if listing:
			print('%5d %s%s link to earlier ino' % (i, t, path))
		return
	seen.add(i)
	extra = ''
	if t in 'cb':
		ad = n['addr']
		dev = (ad[2] << 18 | ad[3]) if ad[1] & 1 else ad[0]
		extra = ' %d,%d' % (dev >> 18, dev & 0x3ffff)
	elif t == 'p':
		pass
	else:
		data = content(i, n)
		if t == 'l':
			extra = ' -> ' + data.decode('latin1')
		if xdir and t in '-l':
			out = os.path.join(xdir, path.lstrip('/'))
			os.makedirs(os.path.dirname(out), exist_ok=True)
			open(out + ('.symlink' if t == 'l' else ''), 'wb').write(data)
	if listing:
		print('%5d %s%s %2d %3d %3d %7d %s%s' % (i, t, oct(n['mode'] & 0o7777)[2:].rjust(4, '0'),
			n['nlink'], n['uid'], n['gid'], n['size'], path, extra))
	if t != 'd':
		return
	if n['size'] % 16:
		bad('%s: directory size %d' % (path, n['size']))
	names = set()
	for k in range(0, n['size'], 16):
		ino = u16(data, k)
		nm = data[k + 2:k + 16].rstrip(b'\0').decode('latin1')
		if ino == 0:
			continue
		if nm in names:
			bad('%s: duplicate entry %s' % (path, nm))
		names.add(nm)
		if not 0 < ino <= ninode:
			bad('%s/%s: ino %d out of range' % (path, nm, ino))
			continue
		if nm == '.':
			if ino != i: bad('%s: . is %d' % (path, ino))
			refs[i] += 1
		elif nm == '..':
			if ino != parent: bad('%s: .. is %d, want %d' % (path, ino, parent))
			refs[ino] = refs.get(ino, 0) + 1
		else:
			visit(ino, path.rstrip('/') + '/' + nm, i)
	if '.' not in names or '..' not in names:
		bad('%s: missing . or ..' % path)

refs[2] = -1		# root's own entry comes from its '..'
visit(2, '/', 2)
for i, n in nodes.items():
	if n['nlink'] != refs[i]:
		bad('ino %d: nlink %d, %d references' % (i, n['nlink'], refs[i]))

used = set(nodes) | {1}
freeino = 0
for i in range(1, ninode + 1):
	m = dinode(i)['mode']
	if m == 0:
		freeino += 1
	elif i not in used:
		bad('ino %d allocated (mode 0%o) but unreachable' % (i, m))
if u16(sb, 436) != freeino:
	bad('s_tinode %d, %d free inodes' % (u16(sb, 436), freeino))
for k in range(u16(sb, 212)):
	i = u16(sb, 214 + 2 * k)
	if not 0 < i <= ninode or dinode(i)['mode'] != 0:
		bad('free-inode cache entry %d (ino %d) not free' % (k, i))

# free list: superblock cache, then chained blocks
nfree, free = u16(sb, 8), [u32(sb, 12 + 4 * k) for k in range(50)]
tfree = 0
while True:
	if not 0 < nfree <= 50:
		bad('free chunk count %d' % nfree)
		break
	for b in free[1:nfree]:
		claim(b, 'free list')
		tfree += 1
	nxt = free[0]
	if nxt == 0:
		break
	claim(nxt, 'free list')
	tfree += 1
	c = blk(nxt)
	nfree, free = u32(c, 0), [u32(c, 4 + 4 * k) for k in range(50)]
if u32(sb, 432) != tfree:
	bad('s_tfree %d, free list holds %d' % (u32(sb, 432), tfree))
lost = fsize - isize - len(owner)
if lost:
	bad('%d blocks neither used nor free' % lost)

print('s5check: %d files, %d data blocks used, %d free blocks, %d free inodes'
	% (len(nodes), len(owner) - tfree, tfree, freeino))
for m in err:
	print('ERROR: ' + m)
print('s5check: %s' % ('FAIL' if err else 'OK'))
sys.exit(1 if err else 0)
