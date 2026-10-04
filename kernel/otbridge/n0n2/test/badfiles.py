#!/usr/bin/env python3
# badfiles.py -- malformed libraries for mkaslm, derived from a good one.
#
#   python3 badfiles.py good.bin outdir [nfuzz]
#
# Writes outdir/<case>.bin: broken MacBinary and AppleDouble containers,
# resource maps, libr/libi/jump table/segment fields and relocation lists,
# then nfuzz (default 0) copies with random bytes changed (fixed seed).
import os, random, struct, sys

good, out = sys.argv[1], sys.argv[2]
nfuzz = int(sys.argv[3]) if len(sys.argv) > 3 else 0
os.makedirs(out, exist_ok=True)
mb = open(good, 'rb').read()
rf = mb[128:128 + struct.unpack('>I', mb[87:91])[0]]

def put(name, data):
    open(os.path.join(out, name + '.bin'), 'wb').write(data)

def w32(b, o, v):
    b[o:o + 4] = struct.pack('>I', v & 0xffffffff)

def w16(b, o, v):
    b[o:o + 2] = struct.pack('>H', v & 0xffff)

def macbin(fork, rlen=None, dlen=0):
    h = bytearray(mb[:128])
    w32(h, 83, dlen)
    w32(h, 87, len(fork) if rlen is None else rlen)
    return bytes(h) + fork

def parse(f):
    doff, moff = struct.unpack('>II', f[:8])
    m = f[moff:]
    tl, nl = struct.unpack('>HH', m[24:28])
    res = []
    for i in range(struct.unpack('>H', m[tl:tl + 2])[0] + 1):
        t = m[tl + 2 + 8 * i:tl + 6 + 8 * i]
        c, ro = struct.unpack('>HH', m[tl + 6 + 8 * i:tl + 10 + 8 * i])
        for j in range(c + 1):
            e = tl + ro + 12 * j
            rid, no, ad = struct.unpack('>hHI', m[e:e + 8])
            d = doff + (ad & 0xffffff)
            ln, = struct.unpack('>I', f[d:d + 4])
            name = None if no == 0xffff else m[nl + no + 1:nl + no + 1 + m[nl + no]]
            res.append([t, rid, ad >> 24, name, bytearray(f[d + 4:d + 4 + ln])])
    return res

def build(res):
    data, types = bytearray(), []
    for r in res:
        if r[0] not in types:
            types.append(r[0])
    refs, names = bytearray(), bytearray()
    tlist = bytearray(struct.pack('>H', len(types) - 1))
    for t in types:
        rs = [r for r in res if r[0] == t]
        tlist += t + struct.pack('>HH', len(rs) - 1, 2 + 8 * len(types) + len(refs))
        for r in rs:
            no = 0xffff
            if r[3] is not None:
                no = len(names)
                names += bytes([len(r[3])]) + r[3]
            refs += struct.pack('>hHII', r[1], no, r[2] << 24 | len(data), 0)
            data += struct.pack('>I', len(r[4])) + r[4]
    m = bytearray(22) + struct.pack('>HHH', 0, 28, 28 + len(tlist) + len(refs))
    m += tlist + refs + names
    hdr = struct.pack('>IIII', 256, 256 + len(data), len(data), len(m))
    return hdr + bytes(240) + bytes(data) + bytes(m)

R = parse(rf)
put('rebuilt_good', macbin(build(R)))

def res_case(name, fn):
    r = [[x[0], x[1], x[2], x[3], bytearray(x[4])] for x in R]
    fn(r)
    put(name, macbin(build(r)))

def get(r, t, rid):
    return [x for x in r if x[0] == t and x[1] == rid][0][4]

libr = get(R, b'libr', 128)
code = bytes(libr[(libr.index(0) + 2) & ~1:][:4])

# containers
put('mb_rsrc_huge', macbin(rf, 0x7fffffff))
put('mb_truncated', macbin(rf)[:300])
put('mb_rsrc_past_end', macbin(rf, len(rf) + 100))
put('mb_data_huge', macbin(rf, None, 0xfffffff0))
ad = bytearray(struct.pack('>II16xH', 0x00051607, 0x00020000, 1))
ad += struct.pack('>III', 2, 38, len(rf))
put('ad_good', bytes(ad) + rf)
a = bytearray(ad); w32(a, 30, 0xfffffff0); w32(a, 34, 0x20)
put('ad_offset_wraps', bytes(a) + rf)
a = bytearray(ad); w32(a, 34, len(rf) + 1)
put('ad_past_end', bytes(a) + rf)
a = bytearray(ad); w16(a, 24, 0xffff)
put('ad_many_entries', bytes(a) + rf[:40])
put('raw_short', rf[:10])

# resource map, edited in place
doff, moff = struct.unpack('>II', rf[:8])
tl, = struct.unpack('>H', rf[moff + 24:moff + 26])
T = moff + tl
def fork_case(name, fn):
    b = bytearray(rf)
    fn(b)
    put(name, macbin(bytes(b)))
fork_case('map_offset_past_end', lambda b: w32(b, 4, len(b) + 16))
fork_case('map_length_huge', lambda b: w32(b, 12, 0xfffffff0))
fork_case('data_length_huge', lambda b: w32(b, 8, 0xfffffff0))
fork_case('typelist_offset', lambda b: w16(b, moff + 24, 0xffff))
fork_case('namelist_offset', lambda b: w16(b, moff + 26, 0xfff0))
fork_case('type_count_huge', lambda b: w16(b, T, 0xfffe))
fork_case('ref_count_huge', lambda b: w16(b, T + 6, 0xfffe))
fork_case('reflist_offset', lambda b: w16(b, T + 8, 0xfff0))
fork_case('ref_data_offset', lambda b: w32(b, T + 2 + 8 * (struct.unpack('>H', rf[T:T + 2])[0] + 1) + 4, 0x00ffffff))
fork_case('res_length_huge', lambda b: w32(b, doff, 0xffffffff))

# library resources
def libr_no_nul(r):
    d = get(r, b'libr', 128)
    d[:] = b'A' * len(d)
res_case('libr_no_nul', libr_no_nul)
res_case('libr_class_count_huge', lambda r: w16(get(r, b'libr', 128), (libr.index(0) + 2 & ~1) + 22, 0xffff))
res_case('libi_count_wraps', lambda r: r.append([b'libi', 128, 0, None, bytearray(struct.pack('>IIII', 0, 0, 0x40000000, 0xfffffff0))]))
res_case('libi_count_large', lambda r: r.append([b'libi', 128, 0, None, bytearray(struct.pack('>IIII', 0, 0, 0x3fffffff, 0xfffffff0))]))
res_case('jt_length_huge', lambda r: w32(get(r, code, 0), 8, 0xfffffff8))
res_case('jt_entry_far', lambda r: w32(get(r, code, 0), 16 + 8 * 2 + 4, 0x7ffffff0))
res_case('seg_a5rel_huge', lambda r: w32(get(r, code, 2), 20, 0x80000000))
res_case('seg_segrel_huge', lambda r: w32(get(r, code, 2), 28, 0xfffffff0))
res_case('seg_short', lambda r: get(r, code, 2).__delitem__(slice(20, None)))
def reloc_huge(r):
    s = get(r, code, 2)
    a5, = struct.unpack('>I', s[20:24])
    s[a5:a5 + 5] = b'\0\x7f\xff\xff\xff'
res_case('reloc_distance_huge', reloc_huge)
def blk(off, v):
    def f(r):
        s = get(r, code, 1)
        w32(s, s.find(b'AXL1') + off, v)
    return f
res_case('blk_below_huge', blk(4, 0x7ffffff0))
res_case('blk_nrel_huge', blk(12, 0x3fffffff))
res_case('blk_initlen_huge', blk(8, 0xfffffff0))
res_case('blk_nsets_huge', blk(16, 0x20000000))
res_case('blk_nrel_wraps', blk(12, 0x40000001))

rnd = random.Random(1)
for k in range(nfuzz):
    b = bytearray(rf)
    for _ in range(rnd.randint(1, 8)):
        i = rnd.randrange(len(b))
        b[i] = rnd.randrange(256)
    put('fuzz%03d' % k, macbin(bytes(b)))
