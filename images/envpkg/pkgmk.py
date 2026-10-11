#!/usr/bin/env python3
# pkgmk.py -- an SVR4 package datastream (pkgmk, then pkgtrans) from a
# prototype file.
#
#   pkgmk.py -r root prototype out.pkg [KEY=value ...]
#
# root holds the files at their installed paths.  Prototype lines, class none:
#   i pkginfo|depend|postinstall|preremove|...=SRC   SRC beside the prototype
#   d none PATH MODE OWNER GROUP
#   f none PATH[=SRC] MODE OWNER GROUP    SRC under root, default PATH
#   e none PATH[=SRC] MODE OWNER GROUP    editable: kept if changed
#   c|b none PATH MAJOR MINOR MODE OWNER GROUP
#   s none PATH=TARGET
# Whatever else root holds goes in as root, group sys: directories and
# executables 0755, other files 0644; system directories stay out.  KEY=value pairs extend pkginfo.
import os, sys

MTIME = 1788094666	# fixed, for reproducible streams
# the system's own: made by pkgadd if missing, never removed by pkgrm
SYSDIRS = ('/dev', '/etc', '/etc/rc2.d', '/home', '/sbin', '/usr', '/usr/bin',
	'/usr/games', '/usr/lib', '/usr/sbin', '/var', '/var/sadm', '/var/sadm/install',
	'/var/sadm/install/admin')

def die(msg):
	sys.exit('pkgmk: ' + msg)

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
	a = sys.argv[1:]
	if len(a) < 4 or a[0] != '-r':
		die('usage: pkgmk.py -r root prototype out.pkg [KEY=value ...]')
	root, proto, outf, extra = a[1], a[2], a[3], a[4:]
	src = lambda p: open(os.path.join(root, p.lstrip('/')), 'rb').read()
	here = os.path.dirname(os.path.abspath(proto))
	inst, ents = {}, {}
	for ln in open(proto):
		f = ln.split('#', 1)[0].split()
		if not f:
			continue
		t = f[0]
		if t == 'i':
			n, s = f[1].split('=', 1)
			inst[n] = open(os.path.join(here, s), 'rb').read()
			continue
		if f[1] != 'none':
			die('%s: class %s' % (f[2], f[1]))
		path, _, s = f[2].partition('=')
		if not path.startswith('/') or path in ents:
			die('%s: bad or repeated path' % path)
		if t == 'd' and len(f) == 6:
			ents[path] = (t, '%s 0%s %s %s' % (path, f[3].lstrip('0'), f[4], f[5]), None)
		elif t in 'fe' and len(f) == 6:
			ents[path] = (t, '0%s %s %s' % (f[3].lstrip('0'), f[4], f[5]), src(s or path))
		elif t in 'cb' and len(f) == 8:
			ents[path] = (t, '%s %s %s 0%s %s %s' % (path, f[3], f[4], f[5].lstrip('0'),
				f[6], f[7]), None)
		elif t == 's' and len(f) == 3 and s:
			ents[path] = (t, '%s=%s' % (path, s), None)
		else:
			die('bad line: ' + ln.strip())
	for d, ds, fs in os.walk(root):
		for n in ds + fs:
			h = os.path.join(d, n)
			path = '/' + os.path.relpath(h, root)
			if path in ents or path in SYSDIRS:
				continue
			if os.path.islink(h) or not (os.path.isdir(h) or os.path.isfile(h)):
				die('%s: not a file or directory' % h)
			if os.path.isdir(h):
				ents[path] = ('d', '%s 0755 root sys' % path, None)
			else:
				ents[path] = ('f', '0%s root sys' % ('755' if os.access(h, os.X_OK) else '644'),
					open(h, 'rb').read())
	if 'pkginfo' not in inst:
		die('no pkginfo')
	info = inst['pkginfo'] + ''.join(x + '\n' for x in extra).encode()
	kv = dict(l.split('=', 1) for l in info.decode().split('\n') if '=' in l)
	pkg = kv['PKG']
	if len(pkg) > 9 or not pkg[0].isalpha():
		die('%s: package abbreviation' % pkg)
	inst['pkginfo'] = info
	m, files, size = [], [], 0
	for path in sorted(ents):
		t, rest, data = ents[path]
		if t in 'fe':
			m.append('1 %s none %s %s %d %d %d' % (t, path, rest, len(data), cksum(data), MTIME))
			files.append(('root' + path, 0o100755 if rest[1:4] != '644' else 0o100644, data))
			size += len(data)
		else:
			m.append('1 %s none %s' % (t, rest))
	for n in sorted(inst):
		m.append('1 i %s %d %d %d' % (n, len(inst[n]), cksum(inst[n]), MTIME))
	blocks = (size + 511) // 512 + len(m) + 4
	pkgmap = (': 1 %d\n' % blocks + '\n'.join(m) + '\n').encode()
	hdr = ('# PaCkAgE DaTaStReAm\n%s 1 %d\n# end of header\n' % (pkg, blocks)).encode()
	s = hdr + b'\0' * (512 - len(hdr))
	s += cpio([(pkg + '/pkginfo', 0o100644, info), (pkg + '/pkgmap', 0o100644, pkgmap)])
	s += b'\0' * (-len(s) % 512)
	# the archive holds each file's parents
	dirs = set()
	for name, _, _ in files:
		p = os.path.dirname(name)
		while p and p not in dirs:
			dirs.add(p)
			p = os.path.dirname(p)
	arc = [('pkginfo', 0o100644, info), ('pkgmap', 0o100644, pkgmap)]
	arc += [(d, 0o40755, b'') for d in sorted(dirs)] + files
	scripts = [('install/' + n, 0o100644, inst[n]) for n in sorted(inst) if n != 'pkginfo']
	if scripts:
		arc += [('install', 0o40755, b'')] + scripts
	s += cpio(arc)
	s += b'\0' * (-len(s) % 512)
	open(outf, 'wb').write(s)

main()
