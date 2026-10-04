# Re-guard a System file's patches for A/UX.  Edits the resource fork of
# an AppleDouble header file (%System) in place.
#  - 'lpch' install groups that patch only Memory Manager traps get the
#    notAUX condition (bit 8).
#  - A 'gpch' that stores a cache routine in the table at $DB8 skips it.
# usage: auxguard.py [-n] %System     (-n: list only)
import sys, os, struct

NOTAUX = 0x100
MM = set(range(0xa01e, 0xa02e)) | {0xa036, 0xa040, 0xa057, 0xa064, 0xa09d}

def fork(d):
    n, = struct.unpack('>H', d[24:26])
    for i in range(n):
        e, o, l = struct.unpack('>III', d[26 + 12 * i:38 + 12 * i])
        if e == 2:
            return o, l
    raise ValueError('no resource fork')

def resources(d, base):
    do, mo = struct.unpack('>II', d[base:base + 8])
    m = base + mo
    tl, = struct.unpack('>H', d[m + 24:m + 26])
    nt, = struct.unpack('>H', d[m + tl:m + tl + 2])
    for i in range(nt + 1):
        t, c, ro = struct.unpack('>4sHH', d[m + tl + 2 + 8 * i:m + tl + 10 + 8 * i])
        for j in range(c + 1):
            r = m + tl + ro + 12 * j
            rid, = struct.unpack('>h', d[r:r + 2])
            off = base + do + (struct.unpack('>I', d[r + 4:r + 8])[0] & 0xffffff)
            ln, = struct.unpack('>I', d[off:off + 4])
            if off + 4 + ln > len(d):
                raise ValueError('resource %d past the end of the file' % rid)
            yield t, rid, off + 4, ln

def groups(b, k):
    # fe, 3-byte condition, then (skip, 2-byte trap) until fe; ff: long skip, 0 ends
    out = []
    while k < len(b):
        if b[k] != 0xfe:
            return None
        at, cond, k, traps = k + 1, int.from_bytes(b[k + 1:k + 4], 'big'), k + 4, []
        while k < len(b) and b[k] != 0xfe:
            s, k = b[k], k + 1
            if s == 0xff:
                s, k = int.from_bytes(b[k:k + 2], 'big'), k + 2
                if s == 0:
                    return out + [(at, cond, traps)] if k == len(b) else None
            traps.append(int.from_bytes(b[k:k + 2], 'big'))
            k += 2
        out.append((at, cond, traps))
    return out if k == len(b) else None

def plausible(g):
    # entries are traps, low-memory or ExpandMem vectors (below $8000)
    # or 0 (code run at install)
    return all(v < 0x8000 or v >= 0xa000 for a, c, t in g for v in t)

def install_table(b):
    # the install groups end the resource; find where they start
    for st in range(len(b)):
        if b[st] == 0xfe:
            g = groups(b, st)
            if g and plausible(g):
                return g
    return []

if __name__ == '__main__':
    a = sys.argv[1:]
    dry = a[:1] == ['-n']
    if dry:
        a = a[1:]
    if len(a) != 1:
        sys.exit('usage: auxguard.py [-n] %System')
    d = bytearray(open(a[0], 'rb').read())
    base, _ = fork(d)
    n = 0
    for t, rid, off, ln in resources(d, base):
        if t != b'lpch':
            continue
        for at, cond, traps in install_table(bytes(d[off:off + ln])):
            # OS trap words carry flag bits 8-10
            if traps and all(v & ~0x700 in MM for v in traps) and not cond & NOTAUX:
                print('lpch %d: %s' % (rid, ' '.join('%04x' % v for v in traps)))
                cond |= NOTAUX
                d[off + at:off + at + 3] = cond.to_bytes(3, 'big')
                n += 1
    # beq to bra before "movea.l $DB8,a1; move.w #124,d0; move.l a0,(a1,d0.w)"
    sig = bytes.fromhex('22780db8303c007c23880000')
    for t, rid, off, ln in resources(d, base):
        k = bytes(d[off:off + ln]).find(sig) if t == b'gpch' else -1
        if k >= 0x1e and d[off + k - 0x1e:off + k - 0x17] == bytes.fromhex('08280000001467'):
            print('gpch %d: $DB8 store' % rid)
            d[off + k - 0x18] = 0x60
            n += 1
    if not dry and n:
        with open(a[0] + '.tmp', 'wb') as f:
            f.write(d)
        os.rename(a[0] + '.tmp', a[0])
