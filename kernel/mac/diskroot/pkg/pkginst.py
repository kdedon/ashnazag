#!/usr/bin/env python3
# pkginst.py -- install SVR4 package datastreams into a disk root as
# pkgadd would: the files, /var/sadm/pkg/PKG and the contents database.
#
#   pkginst.py outdir stream.pkg ...  > manifest lines
#
# One package per stream, class "none", no install scripts; files with
# absolute paths.  outdir receives the files the manifest lines name.
import os, sys

UID = {'root': 0, 'daemon': 1, 'bin': 2, 'sys': 3, 'adm': 4}
GID = {'root': 0, 'other': 1, 'bin': 2, 'sys': 3, 'adm': 4}
INSTDATE = 'Nov 29 1992 00:00'	# the root's fixed build time

def die(msg):
	sys.exit('pkginst: ' + msg)

def cksum(data):
	s = sum(data)
	s = (s & 0xffff) + (s >> 16)
	return (s & 0xffff) + (s >> 16)

def cpio(d, o):
	"""The entries of the SVR4 cpio archive at d[o:] and the end offset."""
	ents = {}
	while True:
		h = d[o:o + 110]
		if h[:6] not in (b'070701', b'070702'):
			die('no cpio header at %d' % o)
		v = [int(h[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
		size, nsz = v[6], v[11]
		name = d[o + 110:o + 110 + nsz - 1].decode('latin1')
		o = (o + 110 + nsz + 3) & ~3
		if name == 'TRAILER!!!':
			return ents, o
		ents[name] = (v[1], d[o:o + size])
		o = (o + size + 3) & ~3

def stream(fname):
	d = open(fname, 'rb').read()
	if not d.startswith(b'# PaCkAgE DaTaStReAm\n'):
		die(fname + ': not a datastream')
	hdr = d[:d.index(b'# end of header')].decode().split('\n')[1:]
	pkgs = [l.split()[0] for l in hdr if l.strip()]
	if len(pkgs) != 1:
		die(fname + ': %d packages' % len(pkgs))
	pkg = pkgs[0]
	meta, o = cpio(d, 512)
	o = (o + 511) & ~511
	files, _ = cpio(d, o)
	return pkg, meta, files

def main():
	out = sys.argv[1]
	contents = {}	# path -> [entry, packages]
	for fname in sys.argv[2:]:
		pkg, meta, files = stream(fname)
		pkginfo = meta[pkg + '/pkginfo'][1].decode()
		pkgmap = meta[pkg + '/pkgmap'][1].decode().split('\n')
		sav = '/var/sadm/pkg/%s' % pkg
		for d in (sav, sav + '/install', sav + '/save'):
			print('d %s 755 0 0' % d)
		for ln in pkgmap[1:]:
			f = ln.split()
			if not f:
				continue
			if f[1] == 'i':
				if f[2] != 'pkginfo':
					die('%s: install file %s' % (pkg, f[2]))
				continue
			part, t, cls, path = f[:4]
			if cls != 'none':
				die('%s: class %s' % (pkg, cls))
			if t in 'sl':
				p, target = path.split('=', 1)
				print('%s %s %s' % ('l' if t == 's' else 'h', p, target))
				entry = '%s=%s %s none' % (p, target, t)
				path = p
			else:
				mode, own, grp = f[4:7]
				m = '%s %s %d %d' % (path, mode, UID[own], GID[grp])
				if t == 'd':
					print('d ' + m)
					entry = '%s d none %s %s %s' % (path, mode, own, grp)
				elif t in 'fev':
					size, ck, mt = f[7:10]
					data = files.get('root' + path, (0, None))[1]
					if data is None or len(data) != int(size) or cksum(data) != int(ck):
						die('%s: %s missing or damaged' % (pkg, path))
					src = os.path.join(out, 'root' + path)
					os.makedirs(os.path.dirname(src), exist_ok=True)
					open(src, 'wb').write(data)
					print('f %s %s' % (m, src))
					# stock pkgadd records an editable file as f, owner marked '\\'
					entry = '%s %s none %s %s %s %s %s %s' % (path, 'f' if t == 'e' else t,
						mode, own, grp, size, ck, mt)
				else:
					die('%s: entry type %s' % (pkg, t))
			if path in contents:
				if contents[path][0] != entry:
					die('%s: %s conflicts' % (pkg, path))
				contents[path][1].append(pkg)
			else:
				contents[path] = [entry, [pkg + '\\' if t == 'e' else pkg]]
		# pkgadd's additions, in its order
		kv = [l for l in pkginfo.split('\n') if l and not l.startswith('CLASSES=')]
		info = os.path.join(out, pkg + '.pkginfo')
		open(info, 'w').write('CLASSES= none\nTZ=GMT0\n'
			'PATH=/sbin:/usr/sbin:/usr/bin:/usr/sadm/install/bin\n'
			'OAMBASE=/usr/sadm/sysadm\n' + ''.join(l + '\n' for l in kv) +
			'PKGINST=%s\nPKGSAV=%s/save\nUPDATE=\nINSTDATE=%s\n' % (pkg, sav, INSTDATE))
		print('f %s/pkginfo 644 0 0 %s' % (sav, info))
	db = os.path.join(out, 'contents')
	open(db, 'w').write('# Last modified by pkginst for %s package\n' % pkg +
		''.join('%s %s\n' % (e, ' '.join(p)) for e, p in
			(contents[k] for k in sorted(contents))))
	print('d /var/sadm/install/logs 755 0 0')
	print('f /var/sadm/install/contents 644 0 0 %s' % db)

main()
