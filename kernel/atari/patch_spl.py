#!/usr/bin/env python3
"""patch_spl.py -- raise the inlined splhi from IPL 4 to IPL 6.

Rewrites every `move.w #$2400,sr` (46fc 2400) in .text to
`move.w #$2600,sr`, so splhi masks the MFP and the SCC.  The count must
match the stock image's.  -c counts; with a count it also fails unless
none is left at IPL 4 and exactly that many are at IPL 6.
"""
import struct, sys

OLD, NEW = bytes.fromhex('46fc2400'), bytes.fromhex('46fc2600')
EXPECT = int(sys.argv[3]) if len(sys.argv) > 3 else None

def text_range(b):
    shoff, = struct.unpack_from('>I', b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('>HHH', b, 0x2e)
    sh = [struct.unpack_from('>IIIIIIIIII', b, shoff + i * shentsize) for i in range(shnum)]
    stro = sh[shstrndx][4]
    for s in sh:
        name = b[stro + s[0]:b.index(b'\0', stro + s[0])]
        if name == b'.text':
            return s[4], s[5]
    sys.exit('no .text')

def main():
    check = sys.argv[1] == '-c'
    path = sys.argv[2]
    b = bytearray(open(path, 'rb').read())
    off, size = text_range(b)
    hits = [i for i in range(off, off + size - 3, 2) if b[i:i + 4] == OLD]
    left = [i for i in range(off, off + size - 3, 2) if b[i:i + 4] == NEW]
    if check:
        print('spl: %d at IPL 4, %d at IPL 6' % (len(hits), len(left)))
        if EXPECT is not None and (hits or len(left) != EXPECT):
            sys.exit('spl: expected 0 at IPL 4, %d at IPL 6' % EXPECT)
        return
    if EXPECT is not None and len(hits) != EXPECT:
        sys.exit('spl: %d sites, expected %d' % (len(hits), EXPECT))
    for i in hits:
        b[i:i + 4] = NEW
    open(path, 'wb').write(b)
    print('spl: %d sites raised to IPL 6' % len(hits))

main()
