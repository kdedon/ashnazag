# fstree.py -- a file tree from a manifest, for the file-system builders.
#
# Manifest lines (octal mode, '#' comments):
#   a ARCHIVE                        every entry of an SVR4 cpio archive (070701/070702)
#   r PATH                           remove PATH (a glob) and everything below it
#   d PATH MODE UID GID              directory
#   f PATH MODE UID GID SRC          regular file, contents from SRCDIR/SRC
#   e PATH MODE UID GID              empty regular file
#   p PATH MODE UID GID              fifo
#   c|b PATH MODE UID GID MAJ MIN    device
#   l PATH TARGET                    symbolic link
#   h PATH EXISTING                  hard link
# A later line for a path replaces the earlier entry; a directory replaced by a
# directory keeps its contents.  Missing parent directories are made 755 root.
# Archive entries keep their owner, mode and mtime; others get the builder's time.
import sys, os, fnmatch

IFMT, IFIFO, IFCHR, IFDIR, IFBLK, IFREG, IFLNK = (0o170000, 0o010000, 0o020000,
	0o040000, 0o060000, 0o100000, 0o120000)

def die(msg):
	sys.exit('%s: %s' % (os.path.basename(sys.argv[0]), msg))

class Ent:
	# t: d f p c b l h; data: bytes (f, l) or link target path (h)
	def __init__(s, t, mode=0, uid=0, gid=0, data=b'', dev=(0, 0), mtime=None):
		s.t, s.mode, s.uid, s.gid, s.data, s.dev, s.mtime = t, mode, uid, gid, data, dev, mtime

def norm(p):
	p = os.path.normpath('/' + p)
	return '/' + p.lstrip('/')

def put(tree, path, e):
	old = tree.get(path)
	if old is not None and not (old.t == 'd' and e.t == 'd'):
		remove(tree, path)
	tree[path] = e

def remove(tree, path):
	for k in [k for k in tree if k == path or k.startswith(path + '/')]:
		del tree[k]

def cpio(tree, fname):
	d = open(fname, 'rb').read()
	o, groups = 0, {}
	while True:
		h = d[o:o + 110]
		if h[:6] not in (b'070701', b'070702'):
			die('%s: no SVR4 cpio header at %d' % (fname, o))
		v = [int(h[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
		ino, mode, uid, gid, nlink, mtime, size, _, _, rmaj, rmin, nsz, _ = v
		name = d[o + 110:o + 110 + nsz - 1].decode('latin1')
		o = (o + 110 + nsz + 3) & ~3
		data = d[o:o + size]
		o = (o + size + 3) & ~3
		if name == 'TRAILER!!!':
			break
		path = norm(name)
		if path == '/':
			continue
		t = {IFDIR: 'd', IFREG: 'f', IFIFO: 'p', IFCHR: 'c', IFBLK: 'b',
			IFLNK: 'l'}.get(mode & IFMT)
		if t is None:
			die('%s: %s: unknown mode 0%o' % (fname, name, mode))
		e = Ent(t, mode & 0o7777, uid, gid, data, (rmaj, rmin), mtime)
		if t != 'd' and nlink > 1:
			# one inode; a file's data travels with one of the names
			first = groups.get(ino)
			if first is not None and tree.get(first) is not None and tree[first].t == t:
				if data:
					tree[first].data = data
				put(tree, path, Ent('h', data=first))
				continue
			groups[ino] = path
		put(tree, path, e)

def load(tree, man, src):
	for ln in open(man):
		f = ln.split('#')[0].split()
		if not f:
			continue
		t = f[0]
		if t == 'a':
			cpio(tree, os.path.join(src, f[1]))
			continue
		path = norm(f[1])
		if t == 'r':
			hits = fnmatch.filter(tree, path)
			if not hits:
				die('nothing to remove: ' + path)
			for p in hits:
				remove(tree, p)
		elif t == 'l':
			put(tree, path, Ent('l', 0o777, data=f[2].encode()))
		elif t == 'h':
			put(tree, path, Ent('h', data=norm(f[2])))
		else:
			e = Ent(t, int(f[2], 8), int(f[3]), int(f[4]))
			if t == 'f':
				e.data = open(os.path.join(src, f[5]), 'rb').read()
			elif t in 'cb':
				e.dev = (int(f[5]), int(f[6]))
			elif t not in 'dep':
				die('unknown line: ' + ln.strip())
			if t == 'e':
				e.t = 'f'
			put(tree, path, e)
	# hard links resolve to the final entry of their target
	for p, e in list(tree.items()):
		if e.t == 'h':
			x = tree.get(e.data)
			if x is None or x.t in 'dh':
				die('hard link %s: bad target %s' % (p, e.data))
	# missing parents
	for p in list(tree):
		d = os.path.dirname(p)
		while d != '/' and d not in tree:
			tree[d] = Ent('d', 0o755)
			d = os.path.dirname(d)
		if d != '/' and tree[d].t != 'd':
			die('%s: parent %s is not a directory' % (p, d))

def tree(man, src):
	t = {'/': Ent('d', 0o755, 0, 0)}
	load(t, man, src)
	return t

def depth_order(tree):
	# parents first, hard links after their targets
	return sorted(tree, key=lambda p: (tree[p].t == 'h', p.count('/'), p))
