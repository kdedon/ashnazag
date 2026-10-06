#!/usr/bin/env python3
# fscheck.py -- read fscheck's images as CP/M 2.2 does and compare each
# file with its source (padded with ^Z to whole records).
#
#   fscheck.py dir
import sys

SKEW6 = [0, 6, 12, 18, 24, 4, 10, 16, 22, 2, 8, 14, 20,
         1, 7, 13, 19, 25, 5, 11, 17, 23, 3, 9, 15, 21]


def geometry(size):
    """spt, block size, dsm, drm, directory blocks, boot tracks, skew"""
    if size == 256256:
        return 26, 1024, 242, 63, 2, 2, SKEW6
    return 128, 4096, size // 4096 - 1, 1023, 8, 0, None


def main():
    d = sys.argv[1]
    want = {}
    for line in open(d + '/fs.lst'):
        img, rest = line.split(' ', 1)
        name, src = rest[:11], rest[12:].rstrip('\n')
        want.setdefault(img, {})[name] = src
    bad = 0
    for img, files in sorted(want.items()):
        data = open(d + '/' + img, 'rb').read()
        spt, bls, dsm, drm, ndir, off, skew = geometry(len(data))
        rpb = bls // 128
        big = dsm > 255
        nptr = 8 if big else 16
        exm = nptr * rpb // 128 - 1

        def rec(r):
            s = r % spt
            o = ((off + r // spt) * spt + (skew[s] if skew else s)) * 128
            return data[o:o + 128]

        dirb = b''.join(rec(r) for r in range((drm + 1) // 4))
        ents = {}
        used = set(range(ndir))
        for i in range(drm + 1):
            e = dirb[32 * i:32 * i + 32]
            if e[0] == 0xe5:
                continue
            if e[0] != 0:
                print('[FAIL] %s: user %d' % (img, e[0])); bad += 1
            name = e[1:12].decode('latin-1')
            ext = (e[12] & 31) | e[14] << 5
            if big:
                ptr = [e[16 + 2 * k] | e[17 + 2 * k] << 8 for k in range(8)]
            else:
                ptr = list(e[16:32])
            nrec = (ext & exm) * 128 + e[15]
            for b in ptr[:(nrec + rpb - 1) // rpb]:
                if b in used or b > dsm:
                    print('[FAIL] %s %s: block %d twice or past the end' % (img, name, b)); bad += 1
                used.add(b)
            ents.setdefault(name, []).append((ext // (exm + 1), nrec, ptr))
        for name, src in files.items():
            exp = open(src, 'rb').read()
            if len(exp) % 128:
                exp += b'\x1a' * (128 - len(exp) % 128)
            got = b''
            for k, (idx, nrec, ptr) in enumerate(sorted(ents.pop(name, []))):
                if idx != k:
                    print('[FAIL] %s %s: entry %d missing' % (img, name, k)); bad += 1
                for r in range(nrec):
                    got += rec(ptr[r // rpb] * rpb + r % rpb)
            if got != exp:
                print('[FAIL] %s %s: %d bytes read, %d expected' % (img, name, len(got), len(exp)))
                bad += 1
        for name in ents:
            print('[FAIL] %s: %s not put there' % (img, name)); bad += 1
        print('[ok] fscheck.py %s: %d files' % (img, len(files)))
    sys.exit(1 if bad else 0)


main()
