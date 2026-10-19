#!/usr/bin/env python3
# prof.py program samples [object.so...] [part.o|lib.a...]: shares of PC samples by part and function
import sys, struct, subprocess, bisect, collections, os
TC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../toolchain/amix')
NM = TC + '/bin/m68k-cbm-sysv4-nm'
SYS = TC + '/m68k-cbm-sysv4/sysroot/usr/lib/libc.so.1'
binary, samples, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
sos = {os.path.basename(f): f for f in rest if f.endswith('.so')}
sos.setdefault('libc.so.1', SYS)
SODEF = {'libGL.so': 'mesa core', 'tdfx_dri.so': 'tdfx', 'r200_dri.so': 'r200', 'libc.so.1': 'libc'}
def nm(*a): return subprocess.run([NM] + list(a), capture_output=True, text=True).stdout.splitlines()
def cat(member, lib):
    m = os.path.basename(member)
    if lib.endswith('libGL.a') or lib.endswith('.o'):
        for p, c in (('glide_', 'glide'), ('x43_tdfx', 'tdfx'), ('x43_r200', 'r200'), ('x43_', 'glx/dri'),
                     ('tnl_', 'mesa tnl'), ('math_', 'mesa tnl'), ('swrast_', 'swrast'), ('X', 'glx/dri')):
            if m.startswith(p): return c
        return 'mesa core'
    if 'libX' in lib: return 'X11'
    if 'libextra' in lib or 'libm' in lib or 'libgcc' in lib: return 'libc'
    return 'app'
owner = {}
for lib in (f for f in rest if not f.endswith('.so')):
    for l in nm('-A', lib):
        f = l.split()
        if len(f) == 3 and f[1] in 'TD':
            mem = f[0].rsplit(':', 2)[-2]  # path[:member]:addr
            owner.setdefault(f[2], cat(mem, lib))
d = open(samples, 'rb').read()
magic, ns, nobj, ut, st, el = struct.unpack('>6I', d[:24])
assert magic == 0x50524f46, 'not a PC sample file'
objs = []
for i in range(nobj):
    base, = struct.unpack('>I', d[24 + 64 * i:28 + 64 * i])
    name = d[28 + 64 * i:88 + 64 * i].split(b'\0')[0].decode()
    objs.append((base, os.path.basename(name)))
pcs = struct.unpack('>%dI' % ns, d[24 + 64 * nobj:24 + 64 * nobj + 4 * ns])
# one sorted table over every object: (address, function, part)
table = []
for base, name in objs:
    path = sos.get(name) if name else binary
    dflt = SODEF.get(name, name) if name else 'app'
    table.append((base, (name or 'program') + ':?', dflt))
    if path is None: continue
    syms = []
    for l in nm('-n', path):
        f = l.split()
        if len(f) == 3 and f[1] in 'TtW':
            a = int(f[0], 16) + base
            # a function at the object's start owns its address
            if f[2].startswith('gcc2_compiled') and syms and syms[-1][0] == a: continue
            syms.append([a, f[2], f[1] in 'TW'])
    # gcc2_compiled. opens each object: name its static code after the object's next symbol
    for i, s in enumerate(syms):
        if s[1].startswith('gcc2_compiled') and i + 1 < len(syms): s[1] = 'static<%s' % syms[i + 1][1]
    cur = dflt
    for a, s, g in syms:
        # globals outside the part sources take the object's part; statics follow the symbol before
        cur = owner.get(s.split('<')[-1], dflt if g else cur)
        table.append((a, (name + ':' if name else '') + s, cur))
table.sort()
addrs = [a for a, _, _ in table]
bypart, bysym = collections.Counter(), collections.Counter()
per = collections.defaultdict(collections.Counter)
for pc in pcs:
    i = bisect.bisect_right(addrs, pc) - 1
    s, p = (table[i][1], table[i][2]) if i >= 0 else ('?', '?')
    bypart[p] += 1; bysym[s] += 1; per[p][s] += 1
n = max(ns, 1)
print('user %d sys %d elapsed %d ticks; %d samples' % (ut, st, el, ns))
for k, v in bypart.most_common(): print('  %-22s %5.1f%%' % (k, 100.0 * v / n))
print('top functions (% of samples):')
for k, v in bysym.most_common(30): print('  %-36s %5.1f%%' % (k, 100.0 * v / n))
if os.environ.get('BYPART'):
    for p, cnt in per.items():
        print('%s:' % p)
        for k, v in cnt.most_common(8): print('  %-36s %5.1f%%' % (k, 100.0 * v / n))
