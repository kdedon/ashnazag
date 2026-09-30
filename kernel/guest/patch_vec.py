#!/usr/bin/env python3
# patch_vec.py -- point the gated M68Kvec slots at their gates.
#
#   patch_vec.py image gates.lst
#
# image: the ld -r kernel with the gates linked in.  Each slot listed
# by mkgates.py must still target the handler recorded there; its
# relocation is retargeted to guest_gate_N.  Idempotent; fails closed.
import struct, sys
from elfrel import Elf

R_68K_32 = 1
e = Elf(sys.argv[1])
base = e.syms[e.sym('M68Kvec')]['value']
rel = e.text_relocs()
n = done = 0
for line in open(sys.argv[2]):
    v, t = line.split()
    v = int(v)
    r = rel.get(base + 4 * v)
    if not r or r[2] != R_68K_32:
        raise SystemExit('ABORT: no R_68K_32 at M68Kvec[%d]' % v)
    g = 'guest_gate_%d' % v
    gi = e.sym(g)
    cur = e.syms[r[1]]['name']
    if cur == g:
        done += 1
        continue
    if cur != t:
        raise SystemExit('ABORT: M68Kvec[%d] is %s, expected %s' % (v, cur, t))
    struct.pack_into('>I', e.b, r[0] + 4, (gi << 8) | R_68K_32)
    n += 1
if n:
    e.write()
print('patch_vec: %d slots retargeted, %d already done' % (n, done))
