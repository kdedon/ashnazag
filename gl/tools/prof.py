#!/usr/bin/env python3
# prof.py binary histogram lib.a... : shares of profil ticks by library part and function
import sys, struct, subprocess, bisect, collections, os
TC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../toolchain/amix')
NM = TC + '/bin/m68k-cbm-sysv4-nm'
binary, samples, libs = sys.argv[1], sys.argv[2], sys.argv[3:]
def cat(member, lib):
    m = os.path.basename(member)
    if lib.endswith('libGL.a'):
        for p, c in (('glide_', 'glide'), ('x43_tdfx', 'tdfx'), ('x43_r200', 'r200'), ('x43_', 'glx/dri'),
                     ('tnl_', 'mesa tnl'), ('math_', 'mesa tnl'), ('swrast_', 'swrast'), ('X', 'glx/dri')):
            if m.startswith(p): return c
        return 'mesa core'
    if 'libX' in lib: return 'X11'
    if 'libextra' in lib or 'libm' in lib or 'libgcc' in lib: return 'libc'
    return 'app'
owner = {}
for lib in libs:
    out = subprocess.run([NM, '-A', lib], capture_output=True, text=True).stdout
    for l in out.splitlines():
        f = l.split()
        if len(f) == 3 and f[1] in 'TD':
            mem = f[0].rsplit(':', 2)[-2]  # path:member:addr
            owner.setdefault(f[2], cat(mem, lib))
syms, glob = [], []
for l in subprocess.run([NM, '-n', binary], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 3 and f[1] in 'Tt':
        a = int(f[0], 16)
        # a function at the object's start owns its address
        if f[2].startswith('gcc2_compiled') and syms and syms[-1][0] == a: continue
        syms.append((a, f[2]))
        glob.append(f[1] == 'T')
# gcc2_compiled. opens each object: name its static code after the object's next symbol
for i, (a, s) in enumerate(syms):
    if s.startswith('gcc2_compiled') and i + 1 < len(syms):
        syms[i] = (a, 'static<%s' % syms[i + 1][1])
addrs = [a for a, _ in syms]
part, cur = [], 'app'
for (a, s), g in zip(syms, glob):
    # globals outside the libraries are the program's; statics follow the symbol before
    cur = owner.get(s.split('<')[-1], 'app' if g else cur)
    part.append(cur)
SYS = TC + '/m68k-cbm-sysv4/sysroot/usr/lib/libc.so.1'
lsyms = []
for l in subprocess.run([NM, '-n', SYS], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 3 and f[1] in 'TtW': lsyms.append((int(f[0], 16) + 0xc1000000, f[2]))
laddrs = [a for a, _ in lsyms]
def sym(pc):
    if pc >= 0xc1000000:
        i = bisect.bisect_right(laddrs, pc) - 1
        return 'libc:' + lsyms[i][1] if i >= 0 else '?'
    i = bisect.bisect_right(addrs, pc) - 1
    return syms[i][1] if i >= 0 else '?'
def partof(pc):
    if pc < addrs[0] or pc >= 0xc1000000: return None
    return part[bisect.bisect_right(addrs, pc) - 1]
d = open(samples, 'rb').read()
magic, lo, nh, ut, st, el = struct.unpack('>6I', d[:24])
assert magic == 0x50524f46, 'not a profil histogram'
h = struct.unpack('>%dH' % nh, d[24:24 + 2 * nh])
n = sum(h)
bypart, bysym = collections.Counter(), collections.Counter()
for i, c in enumerate(h):
    if c:
        pc = lo + 4 * i
        bypart[partof(pc) or '?'] += c; bysym[sym(pc)] += c
print('user %d sys %d elapsed %d ticks; %d in the program, %d (%.1f%% of user) in shared libraries' %
      (ut, st, el, n, ut - n, 100.0 * (ut - n) / max(ut, 1)))
for k, v in bypart.most_common(): print('  %-22s %5.1f%%' % (k, 100.0 * v / max(ut, 1)))
print('top functions (%% of user ticks):')
for k, v in bysym.most_common(30): print('  %-36s %5.1f%%' % (k, 100.0 * v / max(ut, 1)))
if os.environ.get('BYPART'):
    per = collections.defaultdict(collections.Counter)
    for i, c in enumerate(h):
        if c:
            pc = lo + 4 * i
            per[partof(pc) or '?'][sym(pc)] += c
    for p, cnt in per.items():
        print('%s:' % p)
        for k, v in cnt.most_common(8): print('  %-36s %5.1f%%' % (k, 100.0 * v / max(ut, 1)))
