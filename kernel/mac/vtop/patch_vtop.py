#!/usr/bin/env python3
# patch_vtop.py -- 040 base: b_proc only on physio buffers; no legacy
# 030 segment tables in user roots.
#
#   bproc    dma_pageio: the bounce buffer took the request's b_proc
#            although its address is a kernel buffer.  Clear it, as
#            amiga_dma_pageio does, so vtop may trust a nonzero proc.
#   hatmap   hat_map: never grow the legacy segment table.  It sits in
#            root words 8*s, which are live 040 root entries (VA
#            0-0x0fffffff); the 040 tables are built at fault time.
#            The URP load that followed a growth now runs whenever the
#            segment belongs to the current process (exec needs it);
#            fork's segdev_dup maps into the child and must not load it.
#   legacy   i40_on = 0: hat_free no longer reads root entries 4-7 as
#            legacy tables to release; with hatmap there are none, and
#            they may be live 040 entries.
#
# usage: patch_vtop.py [-c] image   (ELF, relocatable or linked; 040 base)
#   -c  check only: exit 1 unless every site is converted.
# Every site's old bytes are checked first; nothing is written unless all
# sites match.  Idempotent.
import struct, subprocess, sys

# (group, symbols tried in order, offset, old, new)
SITES = [
 ('bproc', ('dma_pageio',), 0x60,
  '256b00380038 256b00280028', '42aa0038 4e71 256b00280028'),
 ('hatmap', ('__amix_hat_map', 'hat_map'), 0x88,
  '282effe4 b8aeffdc 6f00005c 2004 5280 2f00 2f2effd4 2f2efffc 4eb9',
  '282effe4 b8aeffdc'
  ' 202efff4'
  ' 207940000730'
  ' b0a8007c'
  ' 6600004e'
  ' 601e'),
 ('legacy', ('i40_on',), 0, '00000001', '00000000'),
]

args = sys.argv[1:]
check = args[:1] == ['-c']
if check:
	args = args[1:]
if len(args) != 1:
	sys.exit('usage: patch_vtop.py [-c] image')
img_path = args[0]
img = bytearray(open(img_path, 'rb').read())
if img[:4] != b'\x7fELF':
	sys.exit('patch_vtop: not an ELF file: %s' % img_path)

shoff, = struct.unpack('>I', img[32:36])
shentsize, shnum, shstrndx = struct.unpack('>HHH', img[46:52])
def sh(i):
	return struct.unpack('>10I', img[shoff + i * shentsize:shoff + i * shentsize + 40])
strtab = sh(shstrndx)
def secname(h):
	o = strtab[4] + h[0]
	return img[o:img.index(b'\0', o)].decode()
secs = dict((secname(sh(i)), sh(i)) for i in range(shnum))

syms, names = {}, set()
nm = subprocess.run(['m68k-elf-nm', img_path], capture_output=True, text=True).stdout
for l in nm.split('\n'):
	f = l.split()
	if len(f) == 3:
		names.add(f[2])
	if len(f) == 3 and f[1] in 'TtWDdV':
		sec = '.text' if f[1] in 'TtW' else '.data'
		syms.setdefault(f[2], []).append((sec, int(f[0], 16)))
if 'cputype' not in names or 'kptr040' not in names:
	sys.exit('patch_vtop: not a 68040/060 port base: %s' % img_path)
if not any(l.split()[1:] == ['A', 'u'] and int(l.split()[0], 16) == 0x40000000
	   for l in nm.split('\n') if len(l.split()) == 3):
	sys.exit('patch_vtop: u is not at 0x40000000: %s' % img_path)

todo, done, bad = [], 0, []
for grp, fns, off, old, new in SITES:
	old, new = bytes.fromhex(old.replace(' ', '')), bytes.fromhex(new.replace(' ', ''))
	assert len(old) == len(new)
	fn = [f for f in fns if f in syms][:1]
	if not fn or len(syms[fn[0]]) != 1:
		sys.exit('patch_vtop: %s not found once' % '/'.join(fns))
	fn = fn[0]
	sec, val = syms[fn][0]
	h = secs[sec]
	fo = h[4] + val - h[3] + off		# sh_offset + value - sh_addr
	cur = bytes(img[fo:fo + len(old)])
	if cur == new:
		done += 1
	elif cur == old:
		todo.append((grp, fn, off, fo, new))
	else:
		bad.append('%s %s+0x%x: %s' % (grp, fn, off, cur.hex()))
if bad:
	sys.exit('patch_vtop: unexpected bytes, nothing written:\n  ' + '\n  '.join(bad))
if check:
	for grp, fn, off, fo, new in todo:
		print('patch_vtop: not converted: %s %s+0x%x' % (grp, fn, off))
	print('patch_vtop: %d/%d sites converted' % (done, len(SITES)))
	sys.exit(1 if todo else 0)
if not todo:
	print('patch_vtop: already applied (%d sites)' % done)
	sys.exit(0)
for grp, fn, off, fo, new in todo:
	img[fo:fo + len(new)] = new
open(img_path, 'wb').write(img)
print('patch_vtop: %d sites converted (%s), %d already' %
      (len(todo), ' '.join(t[0] for t in todo), done))
