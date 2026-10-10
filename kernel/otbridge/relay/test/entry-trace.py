#!/usr/bin/env python3
"""Check entry tracing preserves the caller's registers and stack."""
from pathlib import Path
import struct
import sys

blob = Path(sys.argv[1]).read_bytes()
assert blob[:7] == b'\x7fELF\x01\x02\x01'
at = struct.unpack_from('>I', blob, 32)[0]
size, count, names_index = struct.unpack_from('>HHH', blob, 46)
sections = [struct.unpack_from('>10I', blob, at+i*size) for i in range(count)]
names = sections[names_index]
names = blob[names[4]:names[4]+names[5]]
section = next(s for s in sections if names[s[0]:].split(b'\0',1)[0] == b'.a5init')
code = blob[section[4]:section[4]+section[5]]
def valid(code):
    return (code[:12] == bytes.fromhex('48e7fffe4fefffc0202f0080') and
            code[76:84] == bytes.fromhex('4fef00504cdf7fff') and
            code[84:86] == bytes.fromhex('6000'))
assert valid(code)
for offset, replacement in ((10, b'\x00\x7c'), (78,b'\x00\x4c'), (82,b'\x7f\xfe')):
    changed = bytearray(code)
    changed[offset:offset+2] = replacement
    assert not valid(changed)
assert 15*4 + 64 + 4 == 128
assert 80 == 64 + 4*4
relocations = next(s for s in sections if s[1] == 4 and s[7] == sections.index(section))
symbols = sections[relocations[6]]
strings = sections[symbols[6]]
strings = blob[strings[4]:strings[4]+strings[5]]
for offset in range(relocations[4],relocations[4]+relocations[5],relocations[9]):
    where, info, addend = struct.unpack_from('>IIi',blob,offset)
    if where != 86:
        continue
    assert info & 255 == 5
    index = info >> 8
    name = struct.unpack_from('>I',blob,symbols[4]+index*symbols[9])[0]
    assert strings[name:].split(b'\0',1)[0] == b'__aslm_entry_impl'
    break
else:
    raise AssertionError('missing tail-forward relocation')
print('PASS entry tracer register/stack/tail guards and three corruptions')
