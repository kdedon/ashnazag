#!/usr/bin/env python3
# mkpkg.py -- an SVR4 package datastream (what pkgtrans writes) from a tree.
#
#   mkpkg.py PKG VERSION CATEGORY NAME DESC tree out.pkg
#
# tree holds the files at their installed paths.  One part, class none,
# owner root; executables and directories 0755 (group bin, sys), other
# files 0644.  /usr itself is in the archive, not in the pkgmap.
import os, sys

MTIME = 1788094666	# fixed, for reproducible streams

def cksum(data):
	s = sum(data)
	s = (s & 0xffff) + (s >> 16)
	return (s & 0xffff) + (s >> 16)

def cpio(ents):
	"""SVR4 (newc) cpio of (name, mode, data) entries."""
	out = bytearray()
	for i, (name, mode, data) in enumerate(ents + [('TRAILER!!!', 0, b'')]):
		n = name.encode() + b'\0'
		v = [i + 1, mode, 0, 0, 1 if name == 'TRAILER!!!' else (2 if mode & 0o40000 else 1),
			MTIME, len(data), 0, 0, 0, 0, len(n), 0]
		out += b'070701' + b''.join(b'%08X' % x for x in v) + n
		out += b'\0' * (-len(out) % 4) + data
		out += b'\0' * (-len(out) % 4)
	return bytes(out)

def main():
	pkg, ver, cat, name, desc, tree, outf = sys.argv[1:]
	dirs, files = [], []
	for d, ds, fs in os.walk(tree):
		ds.sort()
		rel = '/' + os.path.relpath(d, tree) if d != tree else ''
		if rel:
			dirs.append(rel)
		for f in sorted(fs):
			p = os.path.join(d, f)
			files.append((rel + '/' + f, open(p, 'rb').read(), os.access(p, os.X_OK)))
	info = ('PKG=%s\nNAME=%s\nARCH=Amiga\nVERSION=%s\nCATEGORY=%s\nCLASSES=none\n'
		'VENDOR=ashnazag\nPSTAMP=ashnazag-%s-%s\nDESC=%s\n' %
		(pkg, name, ver, cat, pkg, ver, desc)).encode()
	m = ['1 d none %s 0755 root sys' % d for d in dirs if d != '/usr']
	m += ['1 f none %s %s root bin %d %d %d' % (p, '0755' if x else '0644', len(b), cksum(b), MTIME)
		for p, b, x in files]
	m.sort(key=lambda l: l.split()[3])
	m.append('1 i pkginfo %d %d %d' % (len(info), cksum(info), MTIME))
	blocks = (sum(len(b) for _, b, _ in files) + 511) // 512 + len(dirs) + 4
	pkgmap = (': 1 %d\n' % blocks + '\n'.join(m) + '\n').encode()
	hdr = ('# PaCkAgE DaTaStReAm\n%s 1 %d\n# end of header\n' % (pkg, blocks)).encode()
	s = hdr + b'\0' * (512 - len(hdr))
	s += cpio([(pkg + '/pkginfo', 0o100644, info), (pkg + '/pkgmap', 0o100644, pkgmap)])
	s += b'\0' * (-len(s) % 512)
	ents = [('pkginfo', 0o100644, info), ('pkgmap', 0o100644, pkgmap), ('root', 0o40755, b'')]
	ents += [('root' + d, 0o40755, b'') for d in dirs]
	ents += [('root' + p, 0o100755 if x else 0o100644, b) for p, b, x in files]
	s += cpio(ents)
	s += b'\0' * (-len(s) % 512)
	open(outf, 'wb').write(s)

main()
