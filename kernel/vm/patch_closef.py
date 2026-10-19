#!/usr/bin/env python3
# patch_closef.py -- xclosef accepts vnodes in the heap chunks.
#
# xclosef panics ("Bad vp", "Bad v_op", "Bad vop_close") unless each
# pointer is in main store or 0x40000000-0x48ffffff.  Heap chunks sit
# above the u-areas, up to 0x7fffffff, so a vnode allocated there
# panicked on close.  Raise the three upper bounds to 0x7fffffff.
#
#   cmpal #0x48ffffff,%a2          b5fc 48ffffff
#   cmpil #0x48ffffff,%a2@(8)      0caa 48ffffff 0008
#   cmpil #0x48ffffff,%a0@(4)      0ca8 48ffffff 0004
#
# usage: patch_closef.py [-c] image   (ELF, relocatable or linked)
#   -c  check only: exit 1 unless all three bounds are raised.
# Kernels without the growing heap (no kptr040) are left alone.  Idempotent.
import struct, subprocess, sys

args = sys.argv[1:]
check = args[:1] == ['-c']
if check:
    args = args[1:]
img_path = args[0]
img = bytearray(open(img_path, 'rb').read())
if img[:4] != b'\x7fELF':
    sys.exit('patch_closef: not an ELF file: %s' % img_path)

nm = subprocess.run(['m68k-elf-nm', '-S', img_path], capture_output=True, text=True).stdout
syms = [l.split() for l in nm.split('\n')]
if not any(s[-1:] == ['kptr040'] for s in syms):
    print('patch_closef: no growing heap, nothing to do')
    sys.exit(0)
fn = [s for s in syms if s[-1:] == ['xclosef'] and len(s) == 4 and s[2] in 'Tt']
if len(fn) != 1:
    sys.exit('patch_closef: xclosef not found once')
addr, size = int(fn[0][0], 16), int(fn[0][1], 16)

shoff, = struct.unpack('>I', img[32:36])
shentsize, shnum, shstrndx = struct.unpack('>HHH', img[46:52])
def sh(i):
    return struct.unpack('>10I', img[shoff + i * shentsize:shoff + i * shentsize + 40])
strtab = sh(shstrndx)
def secname(h):
    o = strtab[4] + h[0]
    return img[o:img.index(b'\0', o)].decode()
text = [sh(i) for i in range(shnum) if secname(sh(i)) == '.text'][0]
base = text[4] + addr - text[3]		# sh_offset + value - sh_addr
body = bytes(img[base:base + size])

sites = [('b5fc', ''), ('0caa', '0008'), ('0ca8', '0004')]
def find(lim):
    r = []
    for op, tail in sites:
        pat = bytes.fromhex(op + lim + tail)
        if body.count(pat) != 1:
            return None
        r.append(body.index(pat) + 2)
    return r
old, new = find('48ffffff'), find('7fffffff')
if new and not old:
    print('patch_closef: applied')
    sys.exit(0)
if check or not old:
    sys.exit('patch_closef: unexpected code in xclosef')
for o in old:
    img[base + o:base + o + 4] = bytes.fromhex('7fffffff')
open(img_path, 'wb').write(img)
print('patch_closef: xclosef bounds raised to 0x7fffffff')
