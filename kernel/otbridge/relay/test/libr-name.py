#!/usr/bin/env python3
"""Require resource names to match library descriptors."""
from pathlib import Path
import struct
import sys


def libraries(blob):
    if len(blob) >= 128 and blob[0] == 0 and 0 < blob[1] < 64:
        data, resource = struct.unpack_from('>II', blob, 83)
        start = 128 + ((data + 127) // 128) * 128
        assert start + resource <= len(blob)
        blob = blob[start:start + resource]
    data, mapping, data_size, map_size = struct.unpack_from('>4I', blob)
    assert data + data_size <= len(blob) and mapping + map_size <= len(blob)
    types, names = struct.unpack_from('>HH', blob, mapping + 24)
    types += mapping; names += mapping
    count = struct.unpack_from('>H', blob, types)[0] + 1
    assert count <= 1024
    found = []
    for i in range(count):
        kind, count, refs = struct.unpack_from('>4sHH', blob, types + 2 + i * 8)
        if kind != b'libr': continue
        assert count < 1024
        for j in range(count + 1):
            at = types + refs + j * 12
            ident, name = struct.unpack_from('>hH', blob, at)
            offset = struct.unpack_from('>I', blob, at + 4)[0] & 0xffffff
            length = struct.unpack_from('>I', blob, data + offset)[0]
            value = blob[data + offset + 4:data + offset + 4 + length]
            assert len(value) == length and b'\0' in value
            library = value.split(b'\0', 1)[0]
            assert name != 0xffff, (ident, library, 'unnamed libr')
            begin = names + name
            assert begin < mapping + map_size
            end = begin + 1 + blob[begin]
            assert end <= mapping + map_size
            assert blob[begin + 1:end] == library, (ident, library, 'mismatched libr name')
            found.append(library.decode('ascii'))
    assert found
    return found


if __name__ == '__main__':
    names = libraries(Path(sys.argv[1]).read_bytes())
    if len(sys.argv) > 2: assert names == [sys.argv[2]], names
    print('PASS named library resources:', ', '.join(names))
