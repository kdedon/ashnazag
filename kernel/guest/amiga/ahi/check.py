#!/usr/bin/env python3
"""Check container.audio's HUNK layout, resident tag and AHI driver vectors."""
import struct
import sys
from pathlib import Path

NVEC = 19  # four library vectors, fifteen AHIsub ones


def check(raw):
    def long(at):
        return struct.unpack_from('>I', raw, at)[0]
    assert [long(i*4) for i in range(5)] == [1011, 0, 1, 0, 0], 'HUNK header'
    size = long(20)*4
    assert long(24) == 1001 and long(28)*4 == size, 'code hunk'
    code = bytearray(raw[32:32+size])
    at = 32+size
    assert long(at) == 1004 and long(at+8) == 0, 'relocation hunk'
    count = long(at+4)
    offsets = [long(at+12+i*4) for i in range(count)]
    assert long(at+12+count*4) == 0 and long(at+16+count*4) == 1010
    for offset in offsets:
        value = struct.unpack_from('>I', code, offset)[0]
        assert offset % 2 == 0 and value <= size
    ptr = lambda a: struct.unpack_from('>I', code, a)[0]
    rt = code.find(b'\x4a\xfc')
    assert rt == 4 and ptr(rt+2) == rt and rt+2 in offsets
    assert code[rt+10:rt+14] == bytes([0x80, 4, 9, 0]), 'RTF_AUTOINIT, version 4, library'
    name = ptr(rt+14)
    assert code[name:name+16] == b'container.audio\0'
    init = ptr(rt+22)
    table = ptr(init+4)
    for i in range(NVEC):
        assert table+4*i in offsets and ptr(table+4*i) < size, 'vector %d' % i
    assert ptr(table+4*NVEC) == 0xffffffff
    return size, count


if __name__ == '__main__':
    size, count = check(Path(sys.argv[1]).read_bytes())
    print(f'[ok] {sys.argv[1]}: {size} code bytes, {count} relocations, {NVEC} vectors')
