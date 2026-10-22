#!/usr/bin/env python3
"""Read a Lisa ProFile volume image (DC42, or raw 532-byte blocks, tag first).

  lisavol.py ls IMAGE            catalog: fileID, pages, name
  lisavol.py get IMAGE NAME OUT  extract a file by name ("-" for stdout)
  lisavol.py boot IMAGE OUT      the boot track (blocks tagged as boot code)
  lisavol.py import IMAGE DEST   every file into folder DEST, see install()

As a module: Vol(path).files() -> {name: fileID}, Vol(path).read(fileID).
"""
import os
import struct
import sys

TAGLEN = 20


class Vol:
    def __init__(self, path):
        d = open(path, 'rb').read()
        if len(d) > 0x54 and d[0x52:0x54] == b'\x01\x00' and d[0] <= 63:
            n = struct.unpack('>I', d[0x40:0x44])[0]
            t = struct.unpack('>I', d[0x44:0x48])[0]
            if n % 512 or t % (n // 512) or 0x54 + n + t > len(d):
                raise ValueError('bad DC42 header')
            self.n = n // 512
            tl = t // self.n
            self.data = d[0x54:0x54 + n]
            tags = d[0x54 + n:0x54 + n + t]
            self.tags = [tags[i * tl:i * tl + tl] for i in range(self.n)]
        elif len(d) % 532 == 0:
            self.n = len(d) // 532
            self.data = b''.join(d[i * 532 + 20:i * 532 + 532] for i in range(self.n))
            self.tags = [d[i * 532:i * 532 + 20] for i in range(self.n)]
        else:
            raise ValueError('not a ProFile image')
        self.byid = {}
        for b, t in enumerate(self.tags):
            fid = struct.unpack('>h', t[4:6])[0]
            self.byid.setdefault(fid, []).append((struct.unpack('>H', t[12:14])[0], b))

    def block(self, b):
        return self.data[b * 512:b * 512 + 512]

    def pages(self, fid):
        """Blocks of a file in page order; where a page repeats, the last one."""
        m = {}
        for p, b in sorted(self.byid.get(fid, [])):
            m[p] = b
        return [m[p] for p in sorted(m)]

    def read(self, fid):
        return b''.join(self.block(b) for b in self.pages(fid))

    def files(self):
        """Catalog entries {name: fileID}. Entries are 64 bytes; a page's
        first entry starts at a page-dependent phase, so each page is
        scanned at the phase that yields the most valid names. The catalog
        keeps stale copies of its pages; the entry whose file is largest wins."""
        best = {}
        for _, b in sorted(self.byid.get(4, [])):
            x = self.block(b)
            top = []
            for r in range(64):
                e = []
                for s in range(r, 512 - 63, 64):
                    nm = x[s + 1:s + 33].split(b'\0')[0]
                    fid = struct.unpack('>H', x[s + 36:s + 38])[0]
                    if len(nm) > 2 and all(32 < c < 127 for c in nm) and fid > 4 and fid in self.byid:
                        e.append((nm.decode('ascii'), fid))
                if len(e) > len(top):
                    top = e
            for nm, fid in top:
                n = len(self.byid[fid])
                if nm not in best or n > best[nm][1]:
                    best[nm] = (fid, n)
        return {nm: v[0] for nm, v in best.items()}

    def mddf(self):
        """The MDDF's block: the first one tagged file 1, where the boot reads it."""
        return min(b for _, b in self.byid.get(1, [(0, -1)]))

    def boot(self):
        """Blocks of the boot track (tag file IDs aaaa/bbbb) and the MDDF's, in block order."""
        return [b for b in range(self.n)
                if self.tags[b][4:6] in (b'\xaa\xaa', b'\xbb\xbb') or b == self.mddf()]

    def chain(self, fid):
        """Blocks of the tag-linked runs of file fid, each from its first page."""
        at = {self.tags[b][8:11]: b for _, b in self.byid.get(fid, [])}
        out = []
        for _, b in sorted(self.byid.get(fid, [])):
            if self.tags[b][17:20] == b'\xff\xff\xff':
                c = [b]
                while self.tags[c[-1]][14:17] in at and len(c) < len(at):
                    c.append(at[self.tags[c[-1]][14:17]])
                out.append(c)
        return out

    def headers(self):
        """{s-file: its header block}: blocks tagged -s, the last of each."""
        h = {}
        for fid, l in self.byid.items():
            if fid < 0 and fid not in (-0x5556, -0x4445):
                h[-fid] = self.block(sorted(l)[-1][1])
        return h

    def entries(self, want):
        """Catalog entries (64 bytes) of the (name, s-file) pairs in want.
        Entries run on across the pages of each catalog chain; of stale
        copies the one modified last wins."""
        e = {}
        for c in self.chain(4):
            x = b''.join(self.block(b) for b in c)
            for o in range(len(x) - 63):
                if x[o] != 0x24:
                    continue
                k = (x[o + 3:o + 35].rstrip(b'\0'), struct.unpack('>H', x[o + 38:o + 40])[0])
                if k in want and (k[1] not in e or x[o + 44:o + 48] > e[k[1]][44:48]):
                    e[k[1]] = x[o:o + 64]
        return e


LISA_EPOCH = 2177452800		# 1901 to 1970, in seconds


def hostname(nm):
    """A Lisa name as a host name: % and / escaped, and a leading dot."""
    h = nm.replace('%', '%25').replace('/', '%2F')
    return '%2E' + h[1:] if h.startswith('.') else h


def install(v, dest):
    """Each file of volume v becomes host file DEST/NAME (its bytes up to
    the logical end) with sidecar DEST/.NAME: "file S", "catalog HEX"
    (its 64-byte entry) and "header HEX" (its header block); the host
    mtime is the modified date.  DEST/.boot holds the boot block, the
    loader's and the MDDF's blocks: 4-byte block number, 20 tag bytes, 512 data each.
    DEST/.volume: "name NAME", "blocks N" and "mddf HEX" (the volume's MDDF block)."""
    hdr = v.headers()
    names = {s: bytes(h[1:1 + h[0]]) for s, h in hdr.items()}
    cat = v.entries({(n, s) for s, n in names.items()})
    os.makedirs(dest, exist_ok=True)
    for s in sorted(hdr):
        if s not in cat:
            sys.exit('lisavol: %s: no catalog entry' % names[s].decode('latin-1'))
        leof = struct.unpack('>I', cat[s][48:52])[0]
        data = v.read(s)
        if sorted({p for p, _ in v.byid.get(s, [])}) != list(range(len(data) // 512)) or len(data) < leof:
            sys.exit('lisavol: %s: %d of %d bytes' % (names[s].decode('latin-1'), len(data), leof))
        h = hostname(names[s].decode('latin-1'))
        p = os.path.join(dest, h)
        with open(p, 'wb') as f:
            f.write(data[:leof])
        with open(os.path.join(dest, '.' + h), 'w') as f:
            f.write('file %d\ncatalog %s\nheader %s\n' % (s, cat[s].hex(), hdr[s].hex()))
        t = struct.unpack('>I', cat[s][44:48])[0]
        if t:
            os.utime(p, (t - LISA_EPOCH, t - LISA_EPOCH))
    with open(os.path.join(dest, '.boot'), 'wb') as f:
        for b in v.boot():
            f.write(struct.pack('>I', b) + v.tags[b][:20].ljust(20, b'\0') + v.block(b))
    m = v.block(v.mddf()) if v.mddf() >= 0 else b''
    if m:
        with open(os.path.join(dest, '.volume'), 'w') as f:
            f.write('name %s\nblocks %d\nmddf %s\n' % (m[13:13 + m[12]].decode('latin-1'), v.n, m[:512].hex()))
    return len(hdr)


def main(a):
    if len(a) < 3 or a[1] not in ('ls', 'get', 'boot', 'import'):
        sys.exit(__doc__)
    v = Vol(a[2])
    if a[1] == 'ls':
        for nm, fid in sorted(v.files().items()):
            print('%5d %5d  %s' % (fid, len(v.pages(fid)), nm))
    elif a[1] == 'get' and len(a) == 5:
        f = v.files()
        k = [n for n in f if n.lower() == a[3].lower()]
        if not k:
            sys.exit('no such file: ' + a[3])
        data = v.read(f[k[0]])
        (sys.stdout.buffer if a[4] == '-' else open(a[4], 'wb')).write(data)
    elif a[1] == 'import' and len(a) == 4:
        print('%d files' % install(v, a[3]))
    elif a[1] == 'boot' and len(a) == 4:
        open(a[3], 'wb').write(b''.join(v.block(b) for b in v.boot()))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
