#!/usr/bin/env python3
"""Check the HUNK loader, resident, and library vector records without execution."""
import struct
import sys
from pathlib import Path


def check(raw):
    def long(at):
        return struct.unpack_from('>I', raw, at)[0]
    assert [long(i*4) for i in range(5)] == [1011, 0, 1, 0, 0], 'HUNK header'
    size = long(20)*4
    assert long(24) == 1001 and long(28)*4 == size, 'code hunk'
    code = bytearray(raw[32:32+size])
    assert len(code) == size
    at = 32+size
    assert long(at) == 1004, 'relocation hunk'
    count = long(at+4)
    assert long(at+8) == 0
    offsets = [long(at+12+i*4) for i in range(count)]
    assert len(set(offsets)) == count
    assert long(at+12+count*4) == 0
    assert long(at+16+count*4) == 1010
    assert at+20+count*4 == len(raw)
    for base in (0x10000, 0x300000):
        relocated = bytearray(code)
        for offset in offsets:
            assert offset % 2 == 0 and offset+4 <= size
            value = struct.unpack_from('>I', code, offset)[0]
            assert value <= size
            struct.pack_into('>I', relocated, offset, value+base)
        def ptr(at):
            value = struct.unpack_from('>I', relocated, at)[0]-base
            assert 0 <= value <= size
            return value
        rt = relocated.find(b'\x4a\xfc')
        assert rt == 4 and ptr(rt+2) == rt
        assert ptr(rt+6) <= size
        assert relocated[rt+10:rt+14] == bytes([0x80, 1, 9, 0])
        name = ptr(rt+14)
        assert relocated[name:name+15] == b'container.card\0'
        init = ptr(rt+22)
        assert struct.unpack_from('>I', relocated, init)[0] == 56
        table = ptr(init+4)
        assert struct.unpack_from('>I', relocated, init+8)[0] == 0
        init_code = ptr(init+12)
        assert init_code < size
        # The core reads CardBase.Name while formatting its interrupt name.
        name_write = relocated.find(b'\x23\x7c', init_code, init_code+24)
        assert name_write >= 0
        assert relocated[name_write+6:name_write+8] == b'\x00\x30'
        board_name = ptr(name_write+2)
        assert relocated[board_name:board_name+20] == b'Amiga Container RTG\0'
        for i in range(6):
            assert ptr(table+4*i) < size
        assert struct.unpack_from('>I', relocated, table+24)[0] == 0xffffffff
    return size, count


if __name__ == '__main__':
    size, count = check(Path(sys.argv[1]).read_bytes())
    print(f'[ok] {sys.argv[1]}: {size} code bytes, {count} relocations, six library vectors')
