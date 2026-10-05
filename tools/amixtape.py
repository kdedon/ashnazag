#!/usr/bin/env python3
"""amixtape.py -- the AMIX 2.1 tape's segments from what the user has.

  amixtape.py extract OUTDIR INPUT...   write segments 00..28 to OUTDIR
  amixtape.py check INPUT...            name the segments found and missing
  amixtape.py table INPUT...            print segments.json entries for INPUT

INPUT: archives (.tar.bz2, .tgz, .tar.gz, .tar.xz, .tar, .zip; one or
several parts), a directory of segment files, a SIMH .tap image, or a raw
image holding the segments back to back.  Segments are recognised by size
and sha256, whatever the member names.  Exit status 1 when a segment
is missing, 2 on unusable input.
"""
import hashlib
import json
import os
import struct
import sys
import tarfile
import zipfile

# segment: (size, sha256), shared with the forge
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                       'webapp', 'amixtape', 'segments.json')) as _f:
    SEGMENTS = {s['id']: (s['size'], s['sha256'])
                for s in json.load(_f)['segments']}

MAXSEG = max(sz for sz, _h in SEGMENTS.values())
BYSIZE = {}
for _n, (_sz, _h) in SEGMENTS.items():
    BYSIZE.setdefault(_sz, []).append(_n)


def die(msg, code=2):
    sys.stderr.write('amixtape: %s\n' % msg)
    sys.exit(code)


def ident(data):
    """The segment number of data, else None."""
    cands = BYSIZE.get(len(data), [])
    if not cands:
        return None
    h = hashlib.sha256(data).hexdigest()
    for n in cands:
        if SEGMENTS[n][1] == h:
            return n
    return None


def members(path, limit=MAXSEG):
    """(name, bytes) for each candidate file in path."""
    if os.path.isdir(path):
        for f in sorted(os.listdir(path)):
            p = os.path.join(path, f)
            if os.path.isfile(p):
                with open(p, 'rb') as fh:
                    yield p, fh.read()
        return
    low = path.lower()
    if low.endswith('.zip'):
        with zipfile.ZipFile(path) as z:
            for i in z.infolist():
                if not i.is_dir() and i.file_size <= limit:
                    yield i.filename, z.read(i)
        return
    if tarfile.is_tarfile(path):
        with tarfile.open(path) as t:
            for i in t:
                if i.isfile() and i.size <= limit:
                    yield i.name, t.extractfile(i).read()
        return
    if low.endswith('.tap'):
        yield from tapfiles(path)
        return
    yield from raw(path)


def tapfiles(path):
    """The files of a SIMH tape image: records between tape marks."""
    with open(path, 'rb') as fh:
        img = fh.read()
    off, cur, k = 0, bytearray(), 0
    while off + 4 <= len(img):
        n = struct.unpack('<I', img[off:off + 4])[0]
        off += 4
        if n >> 28 == 0xf and n != 0xffffffff:
            continue	# gap and reserved markers: no data, no trailer
        if n == 0 or n == 0xffffffff:
            if cur:
                yield '%s:file%d' % (path, k), bytes(cur)
                k += 1
                cur = bytearray()
            if n == 0xffffffff:
                break
            continue
        ln = n & 0xffffff
        if not n & 0x80000000:
            cur += img[off:off + ln]	# error records are skipped
        off += ln + (ln & 1) + 4	# odd records are padded; trailing length
        if off > len(img):
            die('%s: truncated SIMH image' % path)
    if cur:
        yield '%s:file%d' % (path, k), bytes(cur)


def raw(path):
    """A raw image: the segments back to back, in tape order."""
    with open(path, 'rb') as fh:
        img = fh.read()
    off = 0
    for n in sorted(SEGMENTS):
        sz = SEGMENTS[n][0]
        if off + sz > len(img):
            break
        yield '%s@%d' % (path, off), img[off:off + sz]
        off += sz
    if off == 0:
        die('%s: not an archive, directory, .tap or raw AMIX tape' % path)


def scan(inputs, out=None):
    found = {}
    for path in inputs:
        if not os.path.exists(path):
            die('%s: not found' % path)
        for name, data in members(path):
            n = ident(data)
            if n is None or n in found:
                continue
            found[n] = name
            if out:
                p = os.path.join(out, n)
                if not (os.path.isfile(p) and ident(open(p, 'rb').read()) == n):
                    with open(p + '.part', 'wb') as fh:
                        fh.write(data)
                    os.replace(p + '.part', p)
    return found


def report(found):
    miss = [n for n in sorted(SEGMENTS) if n not in found]
    print('segments found: %s' % (' '.join(sorted(found)) or 'none'))
    if miss:
        print('segments missing: %s' % ' '.join(miss))
    return 1 if miss else 0


def main(a):
    if len(a) >= 3 and a[0] == 'extract':
        os.makedirs(a[1], exist_ok=True)
        sys.exit(report(scan(a[2:], a[1])))
    if len(a) >= 2 and a[0] == 'check':
        sys.exit(report(scan(a[1:])))
    if len(a) >= 2 and a[0] == 'table':
        for path in a[1:]:
            for name, data in members(path, 1 << 40):
                b = os.path.basename(name)
                if len(b) == 2 and b.isdigit():
                    print('    ' + json.dumps({'id': b, 'size': len(data),
                          'sha256': hashlib.sha256(data).hexdigest()}) + ',')
        return
    sys.stderr.write(__doc__)
    sys.exit(2)


if __name__ == '__main__':
    main(sys.argv[1:])
