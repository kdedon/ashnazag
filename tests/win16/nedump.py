#!/usr/bin/env python3
# nedump.py -- what an NE (Win16) file is made of: segments, imports by
# module and ordinal (named from apitab.c when given), entry points and
# resources.  A check on our loader's input, and a list of what a
# program needs from the system modules.
#
#   python3 nedump.py [-a apitab.c] FILE...
import re, struct, sys

def w(d, o): return struct.unpack_from('<H', d, o)[0]
def l(d, o): return struct.unpack_from('<I', d, o)[0]

def apinames(path):
    names, mod = {}, None
    for line in open(path):
        m = re.match(r'static struct apient a_(\w+)', line)
        if m:
            mod = m.group(1)
        m = re.match(r'\t\{ (\d+), "([^"]*)"', line)
        if m and mod:
            names[(mod, int(m.group(1)))] = m.group(2)
    return names

def dump(path, names):
    d = open(path, 'rb').read()
    ne = l(d, 0x3c)
    if d[ne:ne + 2] != b'NE':
        print(path, ': not NE'); return
    h = d[ne:]
    nseg, nmod, align = w(h, 0x1c), w(h, 0x1e), w(h, 0x32)
    segt, rest, resn, modt, impt = w(h, 0x22), w(h, 0x24), w(h, 0x26), w(h, 0x28), w(h, 0x2a)
    print('%s: flags %04x, %d segments, CS:IP %d:%04x SS:SP %d:%04x, heap %d stack %d, expected Windows %d.%d'
          % (path, w(h, 0xc), nseg, w(h, 0x16), w(h, 0x14), w(h, 0x1a), w(h, 0x18), w(h, 0x10), w(h, 0x12),
             h[0x3f], h[0x3e]))
    mods = []
    for i in range(nmod):
        o = impt + w(h, modt + 2 * i)
        mods.append(h[o + 1:o + 1 + h[o]].decode('latin1'))
    used = {}
    for s in range(nseg):
        e = segt + 8 * s
        off, ln, fl, mn = w(h, e) << align, w(h, e + 2), w(h, e + 4), w(h, e + 6)
        print('  seg %d: %s %5d bytes (alloc %d)%s' % (s + 1, 'DATA' if fl & 1 else 'CODE', ln or 65536,
              mn or 65536, ' relocs' if fl & 0x100 else ''))
        if fl & 0x100 and off:
            p = off + (ln or 65536)
            for r in range(w(d, p)):
                rec = d[p + 2 + 8 * r:p + 10 + 8 * r]
                kind = rec[1] & 3
                if kind == 1:
                    m = mods[w(rec, 4) - 1]
                    used.setdefault(m, set()).add(w(rec, 6))
                elif kind == 2:
                    m = mods[w(rec, 4) - 1]
                    o = impt + w(rec, 6)
                    used.setdefault(m, set()).add(h[o + 1:o + 1 + h[o]].decode('latin1'))
    for m in sorted(used):
        items = sorted(used[m], key=lambda x: (isinstance(x, str), x))
        print('  %s: %s' % (m, ' '.join(names.get((m, x), str(x)) if isinstance(x, int) else x
                                        for x in items)))

args = sys.argv[1:]
names = {}
if args[:1] == ['-a']:
    names = apinames(args[1]); args = args[2:]
for f in args:
    dump(f, names)
