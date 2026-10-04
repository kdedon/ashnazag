#!/usr/bin/env python3
"""Check loadable hunks and relocate them without executing guest code."""
import struct
import sys
from pathlib import Path


def check(path):
    raw = Path(path).read_bytes()
    assert len(raw) % 4 == 0
    words = list(struct.unpack('>' + 'I' * (len(raw) // 4), raw))
    assert words[:5] == [1011, 0, 1, 0, 0]
    assert words[6] == 1001 and words[5] == words[7]
    size = words[7] * 4
    image = bytearray(raw[32:32 + size])
    at = 8 + words[7]
    relocations = []
    if words[at] == 1004:
        at += 1
        count = words[at]
        assert words[at + 1] == 0
        relocations = words[at + 2:at + 2 + count]
        assert len(relocations) == count and len(set(relocations)) == count
        at += 2 + count
        assert words[at] == 0
        at += 1
    assert words[at:] == [1010]
    for offset in relocations:
        assert offset % 2 == 0 and offset + 4 <= size
        value = struct.unpack_from('>I', image, offset)[0]
        assert 0 <= value < size
        struct.pack_into('>I', image, offset, value + 0x8000000)
    assert size > 64
    print(f'{path}: {size} bytes, {len(relocations)} valid relocations')


if __name__ == '__main__':
    for filename in sys.argv[1:]:
        check(filename)
