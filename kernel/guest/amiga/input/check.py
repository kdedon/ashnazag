#!/usr/bin/env python3
"""Check the command's HUNK boundaries and relocate its internal addresses."""
import struct
import sys
from pathlib import Path
raw = Path(sys.argv[1]).read_bytes()
def word(offset):
    return struct.unpack_from('>I', raw, offset)[0]
assert [word(i * 4) for i in range(5)] == [1011, 0, 1, 0, 0]
size = word(20) * 4
assert word(24) == 1001 and word(28) * 4 == size
code = raw[32:32 + size]
assert len(code) == size and code[:2] == b'\x48\xe7'
assert b'input.device\0' in code and b'dos.library\0' in code
assert b'\x35\x7c\x00\x0b\x00\x1c' in code, 'IND_WRITEEVENT'
at = 32 + size
if word(at) == 1004:
    count = word(at + 4)
    assert word(at + 8) == 0
    offsets = [word(at + 12 + i * 4) for i in range(count)]
    assert len(set(offsets)) == count
    for offset in offsets:
        assert offset % 2 == 0 and offset + 4 <= size
        target = struct.unpack_from('>I', code, offset)[0]
        assert target <= size
    assert word(at + 12 + count * 4) == 0
    at += 16 + count * 4
assert word(at) == 1010 and at + 4 == len(raw)
print(f'[ok] container-input: {size} bytes, bounded HUNK command')
