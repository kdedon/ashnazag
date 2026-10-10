#!/usr/bin/env python3
"""Refuse code that cannot move with its classic resource."""
from pathlib import Path
import struct
import sys

b = Path(sys.argv[1]).read_bytes()
assert b[:7] == b'\x7fELF\x01\x02\x01'
assert struct.unpack_from('>H', b, 18)[0] == 4
at = struct.unpack_from('>I', b, 32)[0]
size, count, strings_index = struct.unpack_from('>HHH', b, 46)
sections = [struct.unpack_from('>10I', b, at + i * size) for i in range(count)]
names = sections[strings_index]
strings = b[names[4]:names[4] + names[5]]
relocations = 0
for section in sections:
    name = strings[section[0]:].split(b'\0', 1)[0].decode()
    if section[2] & 2 and section[5]:
        assert name in ('.text', '.rodata'), f'unsupported allocated section {name}'
    if section[1] in (4, 9):
        for offset in range(section[4], section[4] + section[5], section[9]):
            location, info = struct.unpack_from('>II', b, offset)
            assert info & 255 in (4, 5, 6), f'absolute relocation at {location:#x}'
            relocations += 1
assert relocations, 'linker omitted relocation evidence'
print(f'PASS {relocations} movable code references')
