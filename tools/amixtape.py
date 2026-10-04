#!/usr/bin/env python3
"""amixtape.py -- the AMIX 2.1 tape's segments from what the user has.

  amixtape.py extract OUTDIR INPUT...   write segments 00..28 to OUTDIR
  amixtape.py check INPUT...            name the segments found and missing
  amixtape.py table INPUT...            print a SEGMENTS table for INPUT

INPUT: archives (.tar.bz2, .tgz, .tar.gz, .tar.xz, .tar, .zip; one or
several parts), a directory of segment files, a SIMH .tap image, or a raw
image holding the segments back to back.  Segments are recognised by size
and sha256, whatever the member names.  Exit status 1 when a segment
is missing, 2 on unusable input.
"""
import hashlib
import os
import struct
import sys
import tarfile
import zipfile

# segment: (size, sha256)
SEGMENTS = {
    '00': (5120, 'bec8110855b5c99a60c0d77e0a9da80cd1b921a8357bf02a5dc69764fd986ea2'),
    '01': (2333184, 'a01cbe8ca47c464dd01ebbdca3b835b850492d5b12af953f5424ced5c2abe6c4'),
    '02': (11183104, '9db265fcd103e1b1bbdee98c36cbfee77d777d2185ecc4ae2c6169d806eaf6d5'),
    '03': (1635840, '505c3cc473f067c5cd7bbc13102182c2355720d642078c59752df3106fd5dbd9'),
    '04': (10272768, 'b9b42d30fe6dc92fbf849528f18d8272f98ff6f07e047c383268c4020d2d151c'),
    '05': (2168832, '37ef52daefcc7e5b0ddbfa5318bec3254a809c3fd5c37f84b0b654465d98c75f'),
    '06': (6034944, '1353e932703e58a17f3198ff3f6de91dfa07f323b57681e568523322b9c19118'),
    '07': (3025408, '46e0e53e862e6f9810788c0867af494add807ac66ca4efa797bde71f3512f4d1'),
    '08': (7468544, '442619034ae2c9d1a5d76f628b081ee6b7263097082e44c4a3420046480194fc'),
    '09': (2512384, 'bf2b731b293cb307ed862e4357dc61490343827e613fcf26e3b7a0e5a73b7818'),
    '10': (2023424, '41705a317515c8b3287eaf2f08f27e13b8056fdb28a4048d1c139e0d694df042'),
    '11': (1458688, 'a51cdd93f8dba26c5ac68547c0b5639aca65cdae34751518267b658a911d82ce'),
    '12': (882176, 'b36202be6518c266a0b72ee069f59a52708dc6444c332915110b536415e21fbe'),
    '13': (8095744, '605ab5bb80a0ca2f3c8f7b4a8ba43ca74dec89a4e3a1ee5a970bd236cb3d27cc'),
    '14': (766976, '1126d174d10946cd6223e8a8af6e5a5b16dd85561257c751a1e0a6b1d0f7c1a8'),
    '15': (2192896, '184bba8659a412c321d761ce6f46b8d1c2ae39f84e078ffdcd0a1c967606c9d5'),
    '16': (4470784, '79e8ba1512892b187fa1e714ee81a5d26b03309308da5f0aec2e6833c5b9020f'),
    '17': (2863616, 'c5a18fbe3feeb94834e6df8d280db1cb197f5a3098a834625871ae001da00ec7'),
    '18': (2064384, '6c70b37a51132e12ad8669c55ec2818cefa42d07efe195849323df845814f0ff'),
    '19': (2776576, '1a6d44aade346b1d604df42d1d5d546cbcc3eb576e6dae19c78f31b958b7cf7e'),
    '20': (3334656, '705a7708e828f3c7047feb7986149f416aa086d1c0f1bf3794fe9e136b4b5b49'),
    '21': (2347520, 'f4f8b86706ff56095b2ca6cbbac4973af76215dbdcd4e4b147da4f4937ba18ee'),
    '22': (591360, '5ae9efd1bc90a2693cecd0b692f93704be545cad40582b3127ce663246200713'),
    '23': (4467712, '1b65c1b51fddc26a01dde7b24c1a99a735dab317ec33b7b375f23b23f5c4696c'),
    '24': (8239616, '76d8d55e190d1ba95a1d0b231b6ff044e1d2bd0310af1095f435eeed0af7de3b'),
    '25': (13987328, 'db8ce068376f88e42021b112d691b86d008d1bfa42df260c1197d2686efb534f'),
    '26': (8051200, '2106d2551ac250dfa34e178cabcd4046afb501800e5596e8184311adbc9dee29'),
    '27': (16837120, 'f3c9252819e77cbd2a57762a40131900053be12f8e8243474d8e3d397e995589'),
    '28': (19743744, '29aad0b67c4acaa45379ef53e4877e5da519c3c94af14231a258d78368afe545'),
}

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


def members(path):
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
                if not i.is_dir():
                    yield i.filename, z.read(i)
        return
    if tarfile.is_tarfile(path):
        with tarfile.open(path) as t:
            for i in t:
                if i.isfile():
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
        if n == 0 or n == 0xffffffff:
            if cur:
                yield '%s:file%d' % (path, k), bytes(cur)
                k += 1
                cur = bytearray()
            if n == 0xffffffff:
                break
            continue
        ln = n & 0xffffff
        if n & 0x80000000:
            ln = 0		# error record
        cur += img[off:off + ln]
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
            for name, data in members(path):
                b = os.path.basename(name)
                if len(b) == 2 and b.isdigit():
                    print("    '%s': (%d, '%s')," % (b, len(data),
                          hashlib.sha256(data).hexdigest()))
        return
    sys.stderr.write(__doc__)
    sys.exit(2)


if __name__ == '__main__':
    main(sys.argv[1:])
