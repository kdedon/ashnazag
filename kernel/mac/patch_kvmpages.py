#!/usr/bin/env python3
# patch_kvmpages.py -- page structs and page hash outside the kernel heap.
#
# kvm_init maps the page array and page hash (60 bytes per page plus the
# hash) into kvseg, the fixed 4 MB kernel heap.  At 128 MB that is about
# 2 MB, and large kmem_alloc requests (display shadows) fail.  The array
# already sits in physical clicks reserved at d5; use its address in the
# cached RAM window at 0x60000000 (VA = PA | 0x60000000) instead.
#
#
# usage: patch_kvmpages.py [-c] image   (ELF, relocatable or linked)
#   -c  check only: exit 1 unless both sites are converted.
# Asserts the surrounding code; idempotent.
import struct, subprocess, sys

args = sys.argv[1:]
check = args[:1] == ['-c']
if check:
    args = args[1:]
img_path = args[0]
img = bytearray(open(img_path, 'rb').read())
if img[:4] != b'\x7fELF':
    sys.exit('patch_kvmpages: not an ELF file: %s' % img_path)

shoff, = struct.unpack('>I', img[32:36])
shentsize, shnum, shstrndx = struct.unpack('>HHH', img[46:52])
def sh(i):
    return struct.unpack('>10I', img[shoff + i * shentsize:shoff + i * shentsize + 40])
strtab = sh(shstrndx)
def secname(h):
    o = strtab[4] + h[0]
    return img[o:img.index(b'\0', o)].decode()
text = [sh(i) for i in range(shnum) if secname(sh(i)) == '.text'][0]

nm = subprocess.run(['m68k-elf-nm', img_path], capture_output=True, text=True).stdout
sym = [int(l.split()[0], 16) for l in nm.split('\n')
       if l.split()[-1:] == ['kvm_init'] and l.split()[1] in 'Tt']
if len(sym) != 1:
    sys.exit('patch_kvmpages: kvm_init not found once')
base = text[4] + sym[0] - text[3]		# sh_offset + value - sh_addr

# (offset, old, new); old includes unchanged neighbours as an assertion
sites = [
    (0x23e, bytes.fromhex('ecab' '2805' 'd883' '42a7' '2f05' '48780001' '2f03' '4eb9'),
            bytes.fromhex('ecab' '2805' 'd883' '2005' 'eda8' '008060000000' '0c80')),
    (0x2a2, bytes.fromhex('defc0024' '4eb9'), bytes.fromhex('defc0014' '4eb9')),
]
cur = [bytes(img[base + o:base + o + len(n)]) for o, _, n in sites]
if all(c == n for c, (_, _, n) in zip(cur, sites)):
    print('patch_kvmpages: applied')
    sys.exit(0)
for c, (o, old, _) in zip(cur, sites):
    if check or c != old:
        sys.exit('patch_kvmpages: unexpected code at kvm_init+0x%x: %s' % (o, c.hex()))
for o, _, new in sites:
    img[base + o:base + o + len(new)] = new
open(img_path, 'wb').write(img)
print('patch_kvmpages: page array in the cached RAM window, kvm_init+0x244')
