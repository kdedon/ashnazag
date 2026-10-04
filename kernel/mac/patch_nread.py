#!/usr/bin/env python3
# patch_nread.py -- I_NREAD message count off by one.
#
# strioctl's I_NREAD finds the first data message mp, sets count = 1, then
# counts every message from mp itself on, so mp is counted twice: one
# queued message returns 2.  Start the count at 0 instead.
#
#
# usage: patch_nread.py image     (ELF, relocatable or linked)
# Asserts the surrounding code; idempotent.
import struct, subprocess, sys

img_path = sys.argv[1]
img = bytearray(open(img_path, 'rb').read())
if img[:4] != b'\x7fELF':
    sys.exit('patch_nread: not an ELF file: %s' % img_path)

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
       if l.split()[-1:] == ['strioctl'] and l.split()[1] in 'Tt']
if len(sym) != 1:
    sys.exit('patch_nread: strioctl not found once')
off = text[4] + sym[0] - text[3] + 0x780	# sh_offset + value - sh_addr

old = bytes.fromhex('2d40ffc4' '7801' '584f' '2f06')
new = bytes.fromhex('2d40ffc4' '7800' '584f' '2f06')
cur = bytes(img[off:off + len(old)])
if cur == new:
    print('patch_nread: already applied')
    sys.exit(0)
if cur != old:
    sys.exit('patch_nread: unexpected code at strioctl+0x780: %s' % cur.hex())
img[off:off + len(new)] = new
open(img_path, 'wb').write(img)
print('patch_nread: I_NREAD count starts at 0, strioctl+0x784')
