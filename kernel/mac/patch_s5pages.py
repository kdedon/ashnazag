#!/usr/bin/env python3
# patch_s5pages.py -- s5 file system page geometry for 4 KB VM pages.
#
# The s5 code was compiled for 2 KB pages (PAGESIZE 2048) and runs with
# 4 KB pages.  Each site below is a compiled PAGESIZE, PAGEOFFSET or
# PAGESHIFT, or a policy that only held while a page spanned at most two
# blocks.  One 4 KB page holds 2 blocks (2 KB fs) or 4 blocks (1 KB fs);
# both fit the 4-entry block arrays (page_t p_dblist[], the bn[] frame
# arrays of s5getapage and s5putpage).  A 512-byte fs would need 8, so it
# is refused at mount.
#
#   eof      s5getpage: EOF allowance i_size + PAGEOFFSET.
#   putpage  s5putpage: blocks per page, bsize < PAGESIZE gates, io_len.
#            Unconverted, writeback writes only the lower 2 KB of a page.
#   dblist   s5putpage: always bmap the page's blocks.  p_dblist[] keeps
#            freed block numbers after a truncate that leaves the page
#            cached, and an alloc_only bmap never refreshes it, so the
#            cached list can send data to a block another file now owns.
#   writei   pagecreate tests (offset and length page-aligned) and the
#            tail zero-fill round.  Unconverted, a write from page offset
#            2048 creates the page without reading its lower half.
#   bmap     page_find offset and p_dblist index of a newly allocated
#            block.  Unconverted, page_find gets a 2 KB-aligned offset,
#            finds nothing and the block number is stored through NULL.
#            The bsize > PAGESIZE branch is unreachable for s5 but is
#            converted with it.
#   mount    s5mount, s5mountroot: refuse s_type 1 (512-byte blocks).
#
# usage: patch_s5pages.py [-c] image   (ELF, relocatable or linked)
#   -c  check only: exit 1 unless every site is converted.
# Every site's old bytes are checked first; nothing is written unless all
# sites match.  Idempotent.
import struct, subprocess, sys

# (group, function, offset, old, new)
SITES = [
 ('eof', 's5getpage', 0x40,
  '2005 d083 222a0020 0681000007ff b280',
  '2005 d083 222a0020 068100000fff b280'),

 ('putpage', 's5putpage', 0x92,		# cmpil #PAGEOFFSET,bsize
  '0c87000007ff 6e000010', '0c8700000fff 6e000010'),
 ('putpage', 's5putpage', 0x9c,		# PAGESIZE / bsize
  '2c3c00000800 4c476806', '2c3c00001000 4c476806'),
 ('putpage', 's5putpage', 0x53c,	# io_len = PAGESIZE
  '263c00000800 286effbc', '263c00001000 286effbc'),
 ('putpage', 's5putpage', 0x592,	# io_len += PAGESIZE
  '068300000800 defc0010', '068300001000 defc0010'),
 ('putpage', 's5putpage', 0x5ba,	# p_nio gate
  '0c87000007ff 6e000020', '0c8700000fff 6e000020'),

 ('dblist', 's5putpage', 0x3d0,		# bnew -> bra to the bmap path
  '082c00060001 66000050 4282', '082c00060001 60000050 4282'),

 ('writei', 'writei', 0x24a,		# extend: mapon & PAGEOFFSET
  '2006 0280000007ff 57c0', '2006 028000000fff 57c0'),
 ('writei', 'writei', 0x262,		# overwrite: mapon & PAGEOFFSET
  '2006 0280000007ff 66000010', '2006 028000000fff 66000010'),
 ('writei', 'writei', 0x26e,		# overwrite: n & PAGEOFFSET
  '2005 0280000007ff 66000004', '2005 028000000fff 66000004'),
 ('writei', 'writei', 0x298,		# extend: pagecreate
  '2006 0280000007ff 660000dc', '2006 028000000fff 660000dc'),
 ('writei', 'writei', 0x2f8,		# overwrite: pagecreate, mapon
  '2006 0280000007ff 6600007c', '2006 028000000fff 6600007c'),
 ('writei', 'writei', 0x304,		# overwrite: pagecreate, n
  '2005 0280000007ff 66000070', '2005 028000000fff 66000070'),
 ('writei', 'writei', 0x46a,		# tail zero: roundup(off + n)
  '0680000007ff 0240f800 b0ad0008', '068000000fff 0240f000 b0ad0008'),
 ('writei', 'writei', 0x486,		# tail zero: roundup(mapon + n)
  '0680000007ff 0240f800 2206', '068000000fff 0240f000 2206'),

 ('bmap', 'bmap', 0x298,		# direct: blocks per page
  '243c00000800 4c472802', '243c00001000 4c472802'),
 ('bmap', 'bmap', 0x2b4,		# direct: page of the block
  'e3a3 0243f800 2f03', 'e3a3 0243f000 2f03'),
 ('bmap', 'bmap', 0x2e0,		# direct, bsize > PAGESIZE: pages per block
  '0684000007ff 720b e2a4', '068400000fff 720c e2a4'),
 ('bmap', 'bmap', 0x300,		# direct, bsize > PAGESIZE: page offset
  '2002 720b e3a0', '2002 720c e3a0'),
 ('bmap', 'bmap', 0x59c,		# indirect: blocks per page
  '243c00000800 4c472802', '243c00001000 4c472802'),
 ('bmap', 'bmap', 0x5b2,		# indirect: page of the block
  'e3a0 0240f800 2f00', 'e3a0 0240f000 2f00'),
 ('bmap', 'bmap', 0x5de,		# indirect, bsize > PAGESIZE: pages per block
  '0680000007ff 2800 720b e2a4', '068000000fff 2800 720c e2a4'),
 ('bmap', 'bmap', 0x5fa,		# indirect, bsize > PAGESIZE: page offset
  '2002 720b e3a0', '2002 720c e3a0'),

 # s_type 1: beq to the 512 case -> beq to EINVAL
 ('mount', 's5mount', 0x2da,
  '7201 b280 67000012 60000032', '7201 b280 67000036 60000032'),
 # default (512) case -> moveq #EINVAL,d2; bra to the close/vn_rele exit
 ('mount', 's5mountroot', 0x336,
  '7203 b280 6700001a 297c000002000010 60000016',
  '7203 b280 6700001a 7416 600000f0 4e71 4e71 4e71'),
]

args = sys.argv[1:]
check = args[:1] == ['-c']
if check:
	args = args[1:]
if len(args) != 1:
	sys.exit('usage: patch_s5pages.py [-c] image')
img_path = args[0]
img = bytearray(open(img_path, 'rb').read())
if img[:4] != b'\x7fELF':
	sys.exit('patch_s5pages: not an ELF file: %s' % img_path)

shoff, = struct.unpack('>I', img[32:36])
shentsize, shnum, shstrndx = struct.unpack('>HHH', img[46:52])
def sh(i):
	return struct.unpack('>10I', img[shoff + i * shentsize:shoff + i * shentsize + 40])
strtab = sh(shstrndx)
def secname(h):
	o = strtab[4] + h[0]
	return img[o:img.index(b'\0', o)].decode()
text = [sh(i) for i in range(shnum) if secname(sh(i)) == '.text'][0]

syms = {}
nm = subprocess.run(['m68k-elf-nm', img_path], capture_output=True, text=True).stdout
for l in nm.split('\n'):
	f = l.split()
	if len(f) == 3 and f[1] in 'Tt':
		syms.setdefault(f[2], []).append(int(f[0], 16))

todo, done, bad = [], 0, []
for grp, fn, off, old, new in SITES:
	old, new = bytes.fromhex(old.replace(' ', '')), bytes.fromhex(new.replace(' ', ''))
	assert len(old) == len(new)
	if len(syms.get(fn, [])) != 1:
		sys.exit('patch_s5pages: %s not found once' % fn)
	fo = text[4] + syms[fn][0] - text[3] + off	# sh_offset + value - sh_addr
	cur = bytes(img[fo:fo + len(old)])
	if cur == new:
		done += 1
	elif cur == old:
		todo.append((grp, fn, off, fo, new))
	else:
		bad.append('%s %s+0x%x: %s' % (grp, fn, off, cur.hex()))
if bad:
	sys.exit('patch_s5pages: unexpected code, nothing written:\n  ' + '\n  '.join(bad))
if check:
	for grp, fn, off, fo, new in todo:
		print('patch_s5pages: not converted: %s %s+0x%x' % (grp, fn, off))
	print('patch_s5pages: %d/%d sites converted' % (done, len(SITES)))
	sys.exit(1 if todo else 0)
if not todo:
	print('patch_s5pages: already applied (%d sites)' % done)
	sys.exit(0)
for grp, fn, off, fo, new in todo:
	img[fo:fo + len(new)] = new
open(img_path, 'wb').write(img)
groups = []
for t in todo:
	if t[0] not in groups:
		groups.append(t[0])
print('patch_s5pages: %d sites converted (%s), %d already' % (len(todo), ' '.join(groups), done))
