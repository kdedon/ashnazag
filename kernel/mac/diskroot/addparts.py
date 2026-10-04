#!/usr/bin/env python3
# addparts.py -- append Apple_UNIX_SVR2 root and swap partitions to a disk image
# that has a DDM and an Apple partition map.
#
#   addparts.py disk.img root.img SWAPMB [MB...]
#
# The root partition holds root.img; swap is SWAPMB of zeros.  Both get an
# A/UX block zero block (bzb) with the role A/UX gives them: root/usr flags
# 0xC000 on a type 1 file system, swap as type 3.  Each further MB adds an
# empty partition without a bzb ("Spare1", ...; slices s4 up, for tests).
# The image stays sparse.
import os, struct, sys

BZB_MAGIC = 0xABADBABE

def die(m):
	sys.exit('addparts: ' + m)

def bzb(fstype, flags):
	# magic, cluster, type, inode, flags
	return struct.pack('>IBBHH', BZB_MAGIC, 0, fstype, 1, flags)

def entry(nmap, start, count, name, ptype, bz):
	e = struct.pack('>2sHIII32s32sII', b'PM', 0, nmap, start, count,
		name.encode(), ptype.encode(), 0, count)
	e += struct.pack('>I', 0x37)		# valid, allocated, in use, readable, writable
	return (e.ljust(0x88, b'\0') + bz).ljust(512, b'\0')

def main(a):
	if len(a) < 3:
		die('usage: addparts.py disk.img root.img SWAPMB [MB...]')
	disk, root, swapmb = a[0], a[1], int(a[2])
	extra = [int(x) for x in a[3:]]
	rsz = os.path.getsize(root)
	if rsz % 512:
		die('root image is not a whole number of blocks')
	with open(disk, 'r+b') as f:
		b0 = bytearray(f.read(512))
		if b0[:2] != b'ER' or struct.unpack('>H', b0[2:4])[0] != 512:
			die('no DDM with 512-byte blocks')
		f.seek(512)
		n = struct.unpack('>I', f.read(8)[4:8])[0]
		ents = []
		for i in range(1, n + 1):
			f.seek(512 * i)
			ents.append(bytearray(f.read(512)))
		if n + 2 + len(extra) > 63 or any(e[:2] != b'PM' for e in ents):
			die('bad partition map')
		f.seek(0, 2)
		end = f.tell() // 512
		rstart, rcount = end, rsz // 512
		sstart, scount = rstart + rcount, swapmb * 2048
		total = sstart + scount
		spare = []
		for k, mb in enumerate(extra, 1):
			spare.append(entry(0, total, mb * 2048, 'Spare%d' % k, 'Apple_UNIX_SVR2', b''))
			total += mb * 2048
		ents.append(bytearray(entry(0, rstart, rcount, 'Root', 'Apple_UNIX_SVR2',
			bzb(1, 0xC000))))
		ents.append(bytearray(entry(0, sstart, scount, 'Swap', 'Apple_UNIX_SVR2',
			bzb(3, 0x2000))))
		ents += [bytearray(e) for e in spare]
		for i, e in enumerate(ents, 1):
			struct.pack_into('>I', e, 4, len(ents))	# pmMapBlkCnt
			f.seek(512 * i)
			f.write(e)
		struct.pack_into('>I', b0, 4, total)		# sbBlkCount
		f.seek(0)
		f.write(b0)
		# root data, skipping zero blocks so the image stays sparse
		with open(root, 'rb') as r:
			pos = rstart * 512
			while True:
				c = r.read(1 << 16)
				if not c:
					break
				for k in range(0, len(c), 4096):
					blk = c[k:k + 4096]
					if blk.count(0) != len(blk):
						f.seek(pos + k)
						f.write(blk)
				pos += len(c)
		f.truncate(total * 512)
	print('addparts: root %d + %d, swap %d + %d, %d blocks' % (rstart, rcount,
		sstart, scount, total))

main(sys.argv[1:])
