#!/usr/bin/env python3
# Copy an HFS volume into a smaller one, keeping the catalog as it is.
#
#   hfsshrink.py info IMAGE START COUNT
#   hfsshrink.py shrink IMAGE START COUNT OUT [BLOCKS]
#   hfsshrink.py compare IMAGE START COUNT IMAGE2 START2 COUNT2
#   hfsshrink.py paths LISTING               file paths from "hls -ilaR" output
#
# START and COUNT give the volume's partition in 512-byte blocks.  shrink
# writes a volume of BLOCKS blocks to OUT.  The default is used space + 20%,
# rounded up to 1 MB, and at least 16 MB, which leaves room for a kernel of
# A/UX Startup's 4000 KB limit.  Boot blocks, the catalog and extents B-trees and every
# catalog record are copied unchanged, so catalog node IDs, names, dates,
# Finder info and the blessed folder stay the same.  Only the extent records
# change: each fork is laid out in one extent, with the same physical length.
# The allocation block size stays the same.  The volume must have an empty
# extents overflow tree.
#
# compare checks that both volumes have the same MDB (apart from layout
# fields), the same catalog records (apart from extents) and the same fork
# contents.
import re
import struct
import sys

BLK = 512

# MDB fields that describe the layout (offset, format)
LAYOUT = {0x0e: '>H',   # drVBMSt
          0x10: '>H',   # drAllocPtr
          0x12: '>H',   # drNmAlBlks
          0x1c: '>H',   # drAlBlSt
          0x22: '>H',   # drFreeBks
          0x86: '>12s',  # drXTExtRec
          0x96: '>12s'}  # drCTExtRec


class Vol:
    def __init__(self, path, start, count):
        self.f = open(path, 'rb')
        self.base = start * BLK
        self.count = count
        self.boot = self.read(0, 2 * BLK)
        self.mdb = self.read(2 * BLK, BLK)
        if self.mdb[:2] != b'BD':
            raise ValueError('%s: no HFS volume at block %d' % (path, start))
        (self.vbmst, self.allocptr, self.nblks, self.bsz, _, self.alst,
         _, self.free) = struct.unpack('>HHHIIHIH', self.mdb[14:36])
        self.xtsize = struct.unpack('>I', self.mdb[0x82:0x86])[0]
        self.xtext = exts(self.mdb[0x86:0x92])
        self.ctsize = struct.unpack('>I', self.mdb[0x92:0x96])[0]
        self.ctext = exts(self.mdb[0x96:0xa2])
        self.xt = self.fork(self.xtext, self.xtsize)
        self.ct = self.fork(self.ctext, self.ctsize)
        self.xnodes = btree(self.xt)
        self.cnodes = btree(self.ct)

    def read(self, off, n):
        self.f.seek(self.base + off)
        b = self.f.read(n)
        if len(b) != n:
            raise ValueError('short read')
        return b

    def fork(self, ext, length):
        # the whole physical extent list, then cut to length
        b = b''.join(self.read(self.alst * BLK + s * self.bsz, n * self.bsz)
                     for s, n in ext)
        if len(b) < length:
            raise ValueError('fork extents shorter than its length')
        return b[:length]


def exts(b):
    return [(s, n) for s, n in struct.iter_unpack('>HH', b) if n]


def btree(data):
    # leaf records in key order: (node number, record offset, record length)
    hdr = data[:BLK]
    depth, root, nrecs, fnode, lnode, nsz = struct.unpack('>HIIIIH', hdr[14:34])
    if nsz != BLK:
        raise ValueError('B-tree node size %d' % nsz)
    out, n, seen = [], fnode, set()
    while n:
        if n in seen:
            raise ValueError('B-tree leaf chain loops')
        seen.add(n)
        node = data[n * BLK:(n + 1) * BLK]
        flink, _, typ, _, cnt = struct.unpack('>IIbBH', node[:12])
        if typ != -1:
            raise ValueError('node %d in leaf chain is type %d' % (n, typ))
        offs = [struct.unpack('>H', node[BLK - 2 * (i + 1):BLK - 2 * i])[0]
                for i in range(cnt + 1)]
        for i in range(cnt):
            out.append((n, offs[i], offs[i + 1] - offs[i]))
        n = flink
    if len(out) != nrecs:
        raise ValueError('B-tree has %d leaf records, header says %d' % (len(out), nrecs))
    return out


def catrecs(v):
    # catalog leaf records: (key bytes, data offset in the catalog file, data length)
    out = []
    for n, off, ln in v.cnodes:
        at = n * BLK + off
        kl = v.ct[at]
        doff = (1 + kl + 1) & ~1
        out.append((v.ct[at + 1:at + 1 + kl], at + doff, ln - doff))
    return out


def rec_forks(v, at):
    # a file record's forks: [(name, extent-record offset, logical, physical)]
    r = v.ct
    lg, py = struct.unpack('>II', r[at + 26:at + 34])
    rlg, rpy = struct.unpack('>II', r[at + 36:at + 44])
    return [('data', at + 74, lg, py), ('rsrc', at + 86, rlg, rpy)]


def used_blocks(v):
    n = (v.xtsize + v.ctsize) // v.bsz
    for key, at, ln in catrecs(v):
        if v.ct[at] == 2:
            for _, _, _, py in rec_forks(v, at):
                n += py // v.bsz
    return n


def info(v):
    used = used_blocks(v)
    print('volume %s: %d blocks of %d bytes, %d allocation blocks of %d, first at %d' % (
        v.mdb[37:37 + v.mdb[36]].decode('mac_roman'), v.count, BLK, v.nblks, v.bsz, v.alst))
    print('allocated %d (bitmap free %d), in use by files and B-trees %d = %d bytes' % (
        v.nblks - v.free, v.free, used, used * v.bsz))
    print('extents tree %d leaf records, catalog %d leaf records' % (
        len(v.xnodes), len(v.cnodes)))
    return used


def shrink(v, out, blocks):
    if v.xnodes:
        sys.exit('hfsshrink: extents overflow tree is not empty')
    if len(v.xtext) != 1 or len(v.ctext) != 1:
        sys.exit('hfsshrink: B-tree files are fragmented')
    used = used_blocks(v)
    spb = v.bsz // BLK
    if blocks is None:
        mb = 2048
        blocks = (used * v.bsz * 6 // 5 + mb * BLK - 1) // (mb * BLK) * mb
        blocks = max(blocks, 16 * mb)
    # layout: boot blocks, MDB, bitmap, allocation blocks, alternate MDB, spare
    for bmsecs in range(1, 17):
        nblks = (blocks - v.vbmst - bmsecs - 2) // spb
        if nblks <= bmsecs * BLK * 8:
            break
    alst = v.vbmst + bmsecs
    if nblks < used:
        sys.exit('hfsshrink: %d blocks too small, need %d allocation blocks' % (blocks, used))

    ct = bytearray(v.ct)
    nxt = [0]
    img = bytearray(blocks * BLK)

    def place(data, nab):
        s = nxt[0]
        at = (alst + s * spb) * BLK
        img[at:at + len(data)] = data
        nxt[0] += nab
        return s

    xs = place(v.xt, v.xtsize // v.bsz)
    cs = place(b'', v.ctsize // v.bsz)        # catalog written last
    for key, at, ln in catrecs(v):
        if v.ct[at] != 2:
            continue
        for name, eoff, lg, py in rec_forks(v, at):
            old = exts(v.ct[eoff:eoff + 12])
            if sum(n for _, n in old) * v.bsz != py:
                sys.exit('hfsshrink: fork extents do not match its physical length')
            new = place(v.fork(old, py), py // v.bsz) if py else 0
            ext = struct.pack('>HH', new, py // v.bsz) if py else b''
            ct[eoff:eoff + 12] = ext.ljust(12, b'\0')
            # filStBlk / filRStBlk follow the first extent when they were set
            sb = at + (24 if name == 'data' else 34)
            if old and struct.unpack('>H', ct[sb:sb + 2])[0] == old[0][0]:
                ct[sb:sb + 2] = struct.pack('>H', new)
    at = (alst + cs * spb) * BLK
    img[at:at + len(ct)] = ct
    if nxt[0] != used:
        sys.exit('hfsshrink: placed %d blocks, expected %d' % (nxt[0], used))

    mdb = bytearray(v.mdb)
    struct.pack_into('>H', mdb, 0x0e, v.vbmst)
    struct.pack_into('>H', mdb, 0x10, used)
    struct.pack_into('>H', mdb, 0x12, nblks)
    struct.pack_into('>H', mdb, 0x1c, alst)
    struct.pack_into('>H', mdb, 0x22, nblks - used)
    mdb[0x86:0x92] = struct.pack('>HH', xs, v.xtsize // v.bsz).ljust(12, b'\0')
    mdb[0x96:0xa2] = struct.pack('>HH', cs, v.ctsize // v.bsz).ljust(12, b'\0')
    img[0:2 * BLK] = v.boot
    img[2 * BLK:3 * BLK] = mdb
    img[(blocks - 2) * BLK:(blocks - 1) * BLK] = mdb
    bm = bytearray(bmsecs * BLK)
    for i in range(used):
        bm[i >> 3] |= 0x80 >> (i & 7)
    img[v.vbmst * BLK:alst * BLK] = bm
    open(out, 'wb').write(img)
    print('wrote %s: %d blocks, %d allocation blocks (%d used, %d free), first at %d' % (
        out, blocks, nblks, used, nblks - used, alst))


def compare(a, b):
    bad = []
    if a.boot != b.boot:
        bad.append('boot blocks differ')
    ma, mb = bytearray(a.mdb), bytearray(b.mdb)
    for off, fmt in LAYOUT.items():
        n = struct.calcsize(fmt)
        ma[off:off + n] = mb[off:off + n] = bytes(n)
    if ma != mb:
        bad.append('MDB differs outside the layout fields')
    if a.bsz != b.bsz:
        bad.append('allocation block size differs')
    if used_blocks(a) != used_blocks(b) or used_blocks(b) != b.nblks - b.free:
        bad.append('allocated space differs')
    ra, rb = catrecs(a), catrecs(b)
    if len(ra) != len(rb):
        bad.append('catalog record counts differ')
    files = forks = 0
    for (ka, pa, la), (kb, pb, lb) in zip(ra, rb):
        da, db = bytearray(a.ct[pa:pa + la]), bytearray(b.ct[pb:pb + lb])
        what = repr(ka)
        if ka != kb or la != lb:
            bad.append('catalog key differs at ' + what)
            continue
        if da[0] == 2:
            files += 1
            for (n, ea, lg, py), (_, eb, _, _) in zip(rec_forks(a, pa), rec_forks(b, pb)):
                if a.fork(exts(a.ct[ea:ea + 12]), py) != b.fork(exts(b.ct[eb:eb + 12]), py):
                    bad.append('%s fork differs in %s' % (n, what))
                forks += py > 0
            for off, n in ((24, 2), (34, 2), (74, 24)):
                da[off:off + n] = db[off:off + n] = bytes(n)
        if da != db:
            bad.append('catalog record differs at ' + what)
    print('compared %d catalog records, %d files, %d non-empty forks (physical length), '
          'boot blocks, MDB: %s' % (len(ra), files, forks, 'all identical' if not bad else
                                    '%d differences' % len(bad)))
    for m in bad:
        print('  ' + m)
    return not bad


def paths(listing):
    ent = re.compile(r'^\s*\d+ ([fd])\S*\s.*? [A-Z][a-z]{2} [ \d]\d (?:\d\d:\d\d| \d{4}) (.*)$')
    cur = ':'
    for line in open(listing, encoding='mac_roman'):
        line = line.rstrip('\n')
        if line.startswith(':') and line.endswith(':'):
            cur = line
            continue
        m = ent.match(line)
        if m and m.group(1) == 'f':
            print(cur + m.group(2))
        elif line and not m:
            sys.exit('paths: cannot parse ' + repr(line))


def main(a):
    if a[0] == 'info':
        info(Vol(a[1], int(a[2]), int(a[3])))
    elif a[0] == 'shrink':
        v = Vol(a[1], int(a[2]), int(a[3]))
        info(v)
        shrink(v, a[4], int(a[5]) if len(a) > 5 else None)
    elif a[0] == 'compare':
        if not compare(Vol(a[1], int(a[2]), int(a[3])), Vol(a[4], int(a[5]), int(a[6]))):
            sys.exit(1)
    elif a[0] == 'paths':
        paths(a[1])
    else:
        sys.exit('unknown command ' + a[0])


if __name__ == '__main__':
    main(sys.argv[1:])
