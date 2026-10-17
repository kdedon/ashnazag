#!/usr/bin/env python3
# chkabi.py -- check include/ against the AMIX headers it stands in for.
#
#   python3 kernel/include/chkabi.py [-v]
#
# Every struct size, member offset, typedef size and integer macro in
# include/sys and include/vm is compiled into one probe file, once on these
# headers and once on the AMIX ones (AMIX_KERNEL_CFLAGS from the toolchain's
# env.sh), and the values are compared.  Members named _* are padding.
# Needs the AMIX headers, so it runs locally, not in CI.
import os, re, subprocess, sys, tempfile

INC = os.path.dirname(os.path.abspath(__file__))
AUX = os.path.dirname(os.path.dirname(INC))
ENV = os.path.join(AUX, 'toolchain/src/gcc-cross-amix/build/env.sh')
OWN = ['-O', '-traditional', '-fno-builtin', '-Dinline=__inline__', '-m68020',
       '-D_KERNEL', '-DSVR40', '-DSVR4', '-I' + INC]
# the AMIX headers need their prerequisites first
ORDER = ['types', 'param', 'sysmacros', 'signal', 'resource', 'time', 'user', 'proc',
         'cred', 'systm', 'errno', 'cmn_err', 'kmem', 'conf', 'vnode', 'uio', 'file',
         'exec', 'poll', 'open', 'vfs', 'inline', 'fcntl', 'ioccom', 'mman']
WORK = tempfile.mkdtemp(prefix='chkabi')

def headers():
	hs = []
	for d in ('sys', 'vm'):
		p = os.path.join(INC, d)
		for root, _, files in os.walk(p):
			for f in files:
				if f.endswith('.h'):
					hs.append(os.path.relpath(os.path.join(root, f), INC))
	key = lambda h: (ORDER.index(h[4:-2]) if h[4:-2] in ORDER else len(ORDER), h)
	return sorted(hs, key=key)

def strip(s):
	s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
	return re.sub(r'\\\n', ' ', s)

def probes(h):
	"""(label, expression) pairs for one header."""
	s = strip(open(os.path.join(INC, h)).read())
	out = []
	for m in re.finditer(r'^#define\s+(\w+)\s+(\S.*)$', s, re.M):
		name = m.group(1)
		if not name.startswith('_'):
			out.append((name, '(long)(%s)' % name))
	code = re.sub(r'^#.*$', '', s, flags=re.M)
	for m in re.finditer(r'(typedef\s+)?(struct|union)\s+(\w+)?\s*\{', code):
		i, depth = m.end(), 1
		members, cur = [], ''
		while depth:
			c = code[i]
			if c == '{':
				depth += 1
			elif c == '}':
				depth -= 1
			elif c == ';' and depth == 1:
				members.append(cur)
				cur = ''
			if depth == 1 and c not in '{};':
				cur += c
			elif depth == 1 and c == '}':
				cur = ''
			i += 1
		tail = code[i:code.index(';', i)].strip()
		if m.group(3):
			t = '%s %s' % (m.group(2), m.group(3))
		elif m.group(1) and tail:
			t = tail.split(',')[0].strip()
		else:
			continue
		out.append(('sizeof(%s)' % t, 'sizeof(%s)' % t))
		for mem in members:
			if ':' in mem:
				continue
			fp = re.search(r'\(\s*\*\s*(\w+)\s*\)', mem)
			for d in ([fp.group(1)] if fp else [x.strip() for x in mem.split(',')]):
				n = re.findall(r'(\w+)\s*(?:\[[^]]*\]\s*)*$', d)
				if n and not n[0].startswith('_'):
					out.append(('%s.%s' % (t, n[0]), '((int)&((%s *)0)->%s)' % (t, n[0])))
		if m.group(1) and tail:
			for td in tail.split(','):
				td = td.strip()
				if re.match(r'^\w+$', td):
					out.append(('sizeof(%s)' % td, 'sizeof(%s)' % td))
	for m in re.finditer(r'typedef\s+[^;{}()]*?(\w+)\s*;', code):
		if not m.group(0).lstrip().startswith(('typedef struct', 'typedef union', 'typedef enum')):
			for td in re.findall(r'(\w+)\s*(?:,|$)', m.group(0)[:-1].split(None, 1)[1]):
				out.append(('sizeof(%s)' % td, 'sizeof(%s)' % td))
	return out

def evaluate(cc, env, hs, ps):
	"""label -> value; labels whose probe does not compile are left out."""
	live = list(range(len(ps)))
	src, asm = os.path.join(WORK, 'probe.c'), os.path.join(WORK, 'probe.s')
	while True:
		lines = ['#include <%s>' % h for h in hs]
		base = len(lines) + 1
		for i in live:
			lines.append('int p_%d = %s;' % (i, ps[i][1]))
		open(src, 'w').write('\n'.join(lines) + '\n')
		r = subprocess.run(cc + ['-S', '-o', asm, src], env=env, capture_output=True, text=True)
		if r.returncode == 0:
			break
		bad = {int(x) - base for x in re.findall(r'probe\.c:(\d+):', r.stderr)}
		bad = {live[b] for b in bad if 0 <= b < len(live)}
		if not bad:
			sys.exit('chkabi: probe does not compile:\n' + r.stderr)
		live = [i for i in live if i not in bad]
	vals, label = {}, None
	for line in open(asm):
		m = re.match(r'^p_(\d+):', line)
		if m:
			label = int(m.group(1))
		elif label is not None:
			f = line.split()
			if f:
				vals[ps[label][0]] = f[1]
			label = None
	return vals

def main():
	verbose = '-v' in sys.argv
	env = dict(os.environ)
	out = subprocess.run(['sh', '-c', '. "%s" && env' % ENV], capture_output=True, text=True).stdout
	for l in out.splitlines():
		k, _, v = l.partition('=')
		env[k] = v
	gcc = 'm68k-cbm-sysv4-gcc'
	hs = headers()
	ps, seen = [], set()
	for h in hs:
		for p in probes(h):
			if p[0] not in seen:
				seen.add(p[0])
				ps.append(p)
	kit = evaluate([gcc] + env['AMIX_KERNEL_CFLAGS'].split(), env, hs, ps)
	own = evaluate([gcc] + OWN, dict(env, AMIX_SYSROOT=os.path.join(WORK, 'none')), hs, ps)
	bad = 0
	for name, _ in ps:
		k, o = kit.get(name), own.get(name)
		if k is None and o is None:
			if verbose:
				print('skip', name)
			continue
		if k != o:
			bad += 1
			print('DIFF %s: ours %s, AMIX %s' % (name, o, k))
		elif verbose:
			print('ok', name, o)
	print('chkabi: %d probes, %d differ' % (len(ps), bad))
	return 1 if bad else 0

try:
	r = main()
finally:
	subprocess.run(['rm', '-rf', WORK])
sys.exit(r)
