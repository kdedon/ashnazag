# Edit an AppleDouble header file's resource fork in place.
#  -d TYPE: delete every resource of TYPE (at least one).  The fork must be
#    the file's last entry with the map at its end; the deleted data stays,
#    unreferenced.
#  -c FILE TYPE ID: copy resource TYPE ID from FILE (AppleDouble) over the
#    one of the same size and compression.
#  -a TYPE ID HEX: add resource TYPE ID.
#  -r TYPE ID HEX: replace resource TYPE ID's data, any size; the old data
#    stays, unreferenced.
# usage: rsrcedit.py %file [-d TYPE] [-c FILE TYPE ID] [-a TYPE ID HEX]
#        [-r TYPE ID HEX] ...
import sys, os, struct

def fork(d):
    n, = struct.unpack('>H', d[24:26])
    for i in range(n):
        e, o, l = struct.unpack('>III', d[26 + 12 * i:38 + 12 * i])
        if e == 2:
            return 26 + 12 * i, o, l
    raise ValueError('no resource fork')

def resource(d, typ, rid):
    _, base, _ = fork(d)
    do, mo = struct.unpack('>II', d[base:base + 8])
    m = base + mo
    tl, = struct.unpack('>H', d[m + 24:m + 26])
    nt, = struct.unpack('>H', d[m + tl:m + tl + 2])
    for i in range(nt + 1):
        t, c, ro = struct.unpack('>4sHH', d[m + tl + 2 + 8 * i:m + tl + 10 + 8 * i])
        for j in range(c + 1 if t == typ else 0):
            r = m + tl + ro + 12 * j
            if struct.unpack('>h', d[r:r + 2])[0] == rid:
                off = base + do + (struct.unpack('>I', d[r + 4:r + 8])[0] & 0xffffff)
                ln, = struct.unpack('>I', d[off:off + 4])
                return off + 4, ln, d[r + 4]
    raise ValueError('no %s %d' % (typ.decode('mac_roman'), rid))

def delete(d, typ):
    ent, base, flen = fork(d)
    do, mo, dl, ml = struct.unpack('>IIII', d[base:base + 16])
    m = base + mo
    if base + flen != len(d) or mo + ml != flen:
        raise ValueError('resource map not at the end of the file')
    tl, nl = struct.unpack('>HH', d[m + 24:m + 28])
    nt, = struct.unpack('>H', d[m + tl:m + tl + 2])
    types = []
    for i in range(nt + 1):
        t, c, ro = struct.unpack('>4sHH', d[m + tl + 2 + 8 * i:m + tl + 10 + 8 * i])
        if t != typ:
            types.append((t, c, d[m + tl + ro:m + tl + ro + 12 * (c + 1)]))
    if len(types) == nt + 1:
        raise ValueError('no %s' % typ.decode('mac_roman'))
    # type list, then the reference lists, then the names
    tlist = struct.pack('>H', (len(types) - 1) & 0xffff)
    refs = b''
    for t, c, r in types:
        tlist += struct.pack('>4sHH', t, c, 2 + 8 * len(types) + len(refs))
        refs += r
    names = bytes(d[m + nl:base + flen])
    hdr = bytearray(d[m:m + tl])
    struct.pack_into('>H', hdr, 26, tl + len(tlist) + len(refs))
    mp = bytes(hdr) + tlist + refs + names
    struct.pack_into('>I', d, base + 12, len(mp))
    if struct.unpack('>I', hdr[12:16])[0] == ml:
        struct.pack_into('>I', hdr, 12, len(mp))
        mp = bytes(hdr) + mp[len(hdr):]
    del d[m:]
    d += mp
    struct.pack_into('>I', d, ent + 8, len(d) - base)
    return nt + 1 - len(types)

def add(d, typ, rid, data, name=None):
    ent, base, flen = fork(d)
    do, mo, dl, ml = struct.unpack('>IIII', d[base:base + 16])
    m = base + mo
    # an AppleSingle data fork after the resource fork moves along
    tail = bytes(d[base + flen:])
    del d[base + flen:]
    if mo + ml != flen or mo < do + dl:
        raise ValueError('resource map not at the end, after the data')
    if dl + 4 + len(data) >= 1 << 24:
        raise ValueError('resource data over 16 MB')
    tl, nl = struct.unpack('>HH', d[m + 24:m + 28])
    nt, = struct.unpack('>H', d[m + tl:m + tl + 2])
    types = []
    for i in range(nt + 1):
        t, c, ro = struct.unpack('>4sHH', d[m + tl + 2 + 8 * i:m + tl + 10 + 8 * i])
        types.append([t, c, d[m + tl + ro:m + tl + ro + 12 * (c + 1)]])
    # no attributes, data appended after the old data, the name after the old names
    names = d[m + nl:base + flen]
    ref = struct.pack('>hHII', rid, len(names) if name else 0xffff, dl, 0)
    if name:
        names += bytes([len(name)]) + name
    for e in types:
        if e[0] == typ:
            if any(struct.unpack('>h', e[2][k:k + 2])[0] == rid for k in range(0, len(e[2]), 12)):
                raise ValueError('%s %d already there' % (typ.decode('mac_roman'), rid))
            e[1] += 1
            e[2] += ref
            break
    else:
        types.append([typ, 0, ref])
    tlist = struct.pack('>H', len(types) - 1)
    refs = b''
    for t, c, r in types:
        tlist += struct.pack('>4sHH', t, c, 2 + 8 * len(types) + len(refs))
        refs += r
    hdr = bytearray(d[m:m + tl])
    struct.pack_into('>H', hdr, 26, tl + len(tlist) + len(refs))
    mp = hdr + tlist + refs + names
    del d[m:]
    d[base + do + dl:base + do + dl] = struct.pack('>I', len(data)) + data
    dl += 4 + len(data)
    mo += 4 + len(data)
    struct.pack_into('>IIII', mp, 0, do, mo, dl, len(mp))
    d += mp
    struct.pack_into('>IIII', d, base, do, mo, dl, len(mp))
    struct.pack_into('>I', d, ent + 8, len(d) - base)
    n, = struct.unpack('>H', d[24:26])
    for i in range(n):
        o, = struct.unpack('>I', d[30 + 12 * i:34 + 12 * i])
        if o >= base + flen and tail:
            struct.pack_into('>I', d, 30 + 12 * i, o + len(d) - base - flen)
    d += tail

def replace(d, typ, rid, data):
    ent, base, flen = fork(d)
    do, mo, dl, ml = struct.unpack('>IIII', d[base:base + 16])
    if base + flen != len(d) or mo + ml != flen or mo != do + dl:
        raise ValueError('resource map not right after the data')
    if dl + 4 + len(data) >= 1 << 24:
        raise ValueError('resource data over 16 MB')
    o, _, at = resource(d, typ, rid)
    if at & 1:
        raise ValueError('compressed resource')
    m = base + mo
    tl, = struct.unpack('>H', d[m + 24:m + 26])
    nt, = struct.unpack('>H', d[m + tl:m + tl + 2])
    for i in range(nt + 1):
        t, c, ro = struct.unpack('>4sHH', d[m + tl + 2 + 8 * i:m + tl + 10 + 8 * i])
        for j in range(c + 1 if t == typ else 0):
            r = m + tl + ro + 12 * j
            if struct.unpack('>h', d[r:r + 2])[0] == rid:
                d[r + 5:r + 8] = struct.pack('>I', dl)[1:]
    d[m:m] = struct.pack('>I', len(data)) + data
    dl += 4 + len(data)
    struct.pack_into('>IIII', d, base, do, do + dl, dl, ml)
    struct.pack_into('>II', d, base + do + dl, do, do + dl)
    struct.pack_into('>I', d, base + do + dl + 8, dl)
    struct.pack_into('>I', d, ent + 8, len(d) - base)

if __name__ == '__main__':
    a = sys.argv[2:]
    if len(sys.argv) < 4:
        sys.exit('usage: rsrcedit.py %file [-d TYPE] [-c FILE TYPE ID] [-a TYPE ID HEX] [-r TYPE ID HEX] ...')
    d = bytearray(open(sys.argv[1], 'rb').read())
    while a:
        if a[0] == '-d' and len(a) >= 2:
            delete(d, a[1].encode('mac_roman'))
            a = a[2:]
        elif a[0] == '-c' and len(a) >= 4:
            t, rid = a[2].encode('mac_roman'), int(a[3])
            s = open(a[1], 'rb').read()
            so, sl, sa = resource(s, t, rid)
            o, l, at = resource(d, t, rid)
            if sl != l or (sa ^ at) & 1:
                sys.exit('rsrcedit.py: %s %d: size or compression differs' % (a[2], rid))
            d[o:o + l] = s[so:so + sl]
            a = a[4:]
        elif a[0] == '-a' and len(a) >= 4:
            add(d, a[1].encode('mac_roman'), int(a[2]), bytes.fromhex(a[3]))
            a = a[4:]
        elif a[0] == '-r' and len(a) >= 4:
            replace(d, a[1].encode('mac_roman'), int(a[2]), bytes.fromhex(a[3]))
            a = a[4:]
        else:
            sys.exit('usage: rsrcedit.py %file [-d TYPE] [-c FILE TYPE ID] [-a TYPE ID HEX] [-r TYPE ID HEX] ...')
    with open(sys.argv[1] + '.tmp', 'wb') as f:
        f.write(d)
    os.rename(sys.argv[1] + '.tmp', sys.argv[1])
