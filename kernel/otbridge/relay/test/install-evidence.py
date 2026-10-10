#!/usr/bin/env python3
"""Check locally extracted shipping cd70 install metadata."""
import struct
import sys
from pathlib import Path


def decode(raw, start):
    below, version, reserved, copy, relocation = struct.unpack_from('>IHHII', raw, start)
    assert version == 1 and reserved == 0
    output = bytearray(below)
    cursor, dest, repeat = start + copy, 0, 1

    def number(depth=0):
        nonlocal cursor, repeat
        assert depth < 8 and cursor < len(raw)
        n = raw[cursor]
        cursor += 1
        if n < 128:
            return n
        if n < 240:
            width = 1 if n < 192 else 2 if n < 224 else 4
            assert cursor + width <= len(raw)
            prefix = n & 63 if width == 1 else n & 31 if width == 2 else 0
            value = (prefix << (width * 8)) | int.from_bytes(raw[cursor:cursor + width], 'big')
            cursor += width
            return value
        value = number(depth + 1)
        repeat = number(depth + 1)
        assert repeat <= below
        return value

    while True:
        assert cursor < len(raw)
        code = raw[cursor]
        cursor += 1
        repeat = 1
        length = (code & 15) * 2 if code & 15 else number()
        if not length:
            break
        skip = (code & 240) >> 3 if code & 240 else number()
        for _ in range(repeat):
            dest += skip
            assert dest + length <= below and cursor + length <= len(raw)
            output[dest:dest + length] = raw[cursor:cursor + length]
            cursor += length
            dest += length
    assert cursor == start + relocation
    return output


def getter(data, jumps, code, name):
    position = data.index(name.encode() + b'\0')
    nameptr = position - len(data)
    refs = [o for o in range(0, len(data) - 3, 2)
            if struct.unpack_from('>i', data, o)[0] == nameptr]
    assert len(refs) == 1
    names = refs[0]
    assert struct.unpack_from('>I', data, names + 4)[0] == 0
    assert struct.unpack_from('>i', data, names + 8)[0] == names - len(data)
    pointer = struct.unpack_from('>I', data, names + 12)[0]
    above, below, length, off = struct.unpack_from('>4I', jumps)
    assert off == 0x20 and pointer >= off + 2
    raw = 16 + pointer - off - 2
    segment, trap, target = struct.unpack_from('>HHI', jumps, raw)
    assert segment == 2 and trap == 0xa9f0 and target + 16 <= len(code)
    assert code[target:target + 6] == bytes.fromhex('4e56000041f9')
    displacement = struct.unpack_from('>i', code, target + 6)[0]
    return target, len(data) + displacement


def main():
    init, jumps, code = (Path(arg).read_bytes() for arg in sys.argv[1:])
    data = decode(init, 1694)
    assert len(data) == 0x4b0a
    for name, flags, sync in [('tcp', 0x1001, 3), ('udp', 0x1001, 1)]:
        target, offset = getter(data, jumps, code, 'GetOT' + name + 'InstallInfo')
        table, found_flags, found_sync, buddy, loads, references = struct.unpack_from('>i5I', data, offset)
        assert (found_flags, found_sync, buddy, loads, references) == (flags, sync, 0, 0, 0)
        read, write, muxread, muxwrite = struct.unpack_from('>4i', data, len(data) + table)
        assert read < 0 and write < 0 and not muxread and not muxwrite
        rinit = struct.unpack_from('>7i', data, len(data) + read)
        winit = struct.unpack_from('>7i', data, len(data) + write)
        assert rinit[0] and rinit[2] and rinit[3] and rinit[5] and winit[0] and winit[5]
        print(f'PASS named shipping {name}: getter={target:#x} install={offset} flags={flags:#x} sync={sync}; rawJT=16+A5pointer-off-2')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit('usage: install-evidence.py cd70-1.bin cd70-0.bin cd70-2.bin')
    main()
