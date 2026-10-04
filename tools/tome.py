# List and extract files from an Installer 4 tome ('idcp'/'kakc', data fork).
#
# Tome layout (all big-endian):
#   0x00  'kc' 0001, ..., 0x16 dir offset (long), 0x1a entry count (word)
#   dir   0x80-byte entries:
#     +0x06 name (Str31)   +0x26 type  +0x2a creator  +0x2e crdate
#     +0x32 moddate        +0x36 version (BCD), stage  +0x3a Finder flags
#     +0x3c data fork:  length, offset, stored length, checksum
#     +0x4c rsrc fork:  length, offset, stored length, checksum
#   fork  64 KB chunks, each after a 4-byte header 01 08 02 a6 (stored)
#
# usage: tome.py list TOME.bin [SCRIPT.bin]
#        tome.py extract TOME.bin OUTDIR NAME...
import sys, os, struct
from macbin2ad import ad_header

STORED = b'\x01\x08\x02\xa6'
CHUNK = 0x10000

def macbinary(path):
    b = open(path, 'rb').read()
    if len(b) < 128 or b[0] != 0 or not 1 <= b[1] <= 63 or b[74] != 0:
        raise ValueError('%s: not MacBinary' % path)
    dlen, rlen = struct.unpack('>II', b[83:91])
    roff = 128 + (dlen + 127) // 128 * 128
    return b[128:128 + dlen], b[roff:roff + rlen]

def entries(d):
    if d[:4] != b'kc\0\1':
        raise ValueError('not a tome')
    dir, n = struct.unpack('>IH', d[0x16:0x1c])
    for i in range(n):
        e = d[dir + 0x80 * i:dir + 0x80 * (i + 1)]
        yield {'name': e[7:7 + e[6]], 'type': e[0x26:0x2a],
               'creator': e[0x2a:0x2e], 'flags': e[0x3a:0x3c],
               'data': struct.unpack('>III', e[0x3c:0x48]),
               'rsrc': struct.unpack('>III', e[0x4c:0x58])}

def fork(d, f):
    ln, off, stored = f
    out = bytearray()
    end = off + stored
    while len(out) < ln:
        if d[off:off + 4] != STORED:
            raise ValueError('unknown chunk header %s at 0x%x'
                             % (d[off:off + 4].hex(), off))
        n = min(CHUNK, ln - len(out))
        out += d[off + 4:off + 4 + n]
        off += 4 + n
    if off != end or len(out) != ln:
        raise ValueError('fork size mismatch')
    return bytes(out)

def kind(d, f):
    if not f[2]:
        return '-'
    chunks = (f[0] + CHUNK - 1) // CHUNK
    return 'stored' if f[2] == f[0] + 4 * chunks else 'other'

# A script 'infa' resource holds the install target "Folder: Name" as a
# Pascal string at offset 32; files in the System Folder have no folder part.
def targets(script):
    folders = {}
    for rid, x in resources(script).get(b'infa', []):
        s = x[33:33 + x[32]] if len(x) > 32 else b''
        folder, _, name = s.rpartition(b':')
        if name:
            folders.setdefault(name.lstrip(), set()).add(folder or b'System Folder')
    return folders

def resources(r):
    do, mo = struct.unpack('>II', r[:8])
    m = r[mo:]
    tl, = struct.unpack('>H', m[24:26])
    nt, = struct.unpack('>H', m[tl:tl + 2])
    out = {}
    for i in range(nt + 1):
        t, c, ro = struct.unpack('>4sHH', m[tl + 2 + 8 * i:tl + 10 + 8 * i])
        for j in range(c + 1):
            e = m[tl + ro + 12 * j:tl + ro + 12 * j + 12]
            rid, = struct.unpack('>h', e[:2])
            off = do + (struct.unpack('>I', e[4:8])[0] & 0xffffff)
            ln, = struct.unpack('>I', r[off:off + 4])
            out.setdefault(t, []).append((rid, r[off + 4:off + 4 + ln]))
    return out

def extract(d, e, dir):
    name = e['name']
    if not 0 < len(name) < 32 or b'/' in name or b'\0' in name or name in (b'.', b'..'):
        raise ValueError('bad file name %r' % name)
    data, rsrc = fork(d, e['data']), fork(d, e['rsrc'])
    # drop "inited" so the Finder places the icon itself
    flags = struct.unpack('>H', e['flags'])[0] & ~0x0100
    finfo = e['type'] + e['creator'] + struct.pack('>HIH', flags, 0, 0)
    p = os.fsencode(dir)
    with open(os.path.join(p, name), 'wb') as f:
        f.write(data)
    with open(os.path.join(p, b'%' + name), 'wb') as f:
        f.write(ad_header(finfo, len(rsrc)) + rsrc)

def show(b):
    return b.decode('mac_roman').replace('\r', '\\r')

def main(a):
    if len(a) >= 2 and a[0] == 'list' and len(a) <= 3:
        d, _ = macbinary(a[1])
        where = targets(macbinary(a[2])[1]) if len(a) == 3 else {}
        for e in entries(d):
            print('%-32s %s/%s %8d %8d %-6s %s' % (
                show(e['name']), show(e['type']), show(e['creator']),
                e['data'][0], e['rsrc'][0],
                kind(d, e['rsrc'] if e['rsrc'][2] else e['data']),
                ', '.join(sorted(show(f) for f in where.get(e['name'], ())))))
    elif len(a) >= 4 and a[0] == 'extract':
        d, _ = macbinary(a[1])
        want = [n.encode('mac_roman') for n in a[3:]]
        found = [e for e in entries(d) if e['name'] in want]
        for e in found:
            extract(d, e, a[2])
        missing = set(want) - set(e['name'] for e in found)
        if missing:
            sys.exit('not in tome: ' + ', '.join(sorted(map(show, missing))))
    else:
        sys.exit('usage: tome.py list TOME.bin [SCRIPT.bin]\n'
                 '       tome.py extract TOME.bin OUTDIR NAME...')

if __name__ == '__main__':
    main(sys.argv[1:])
