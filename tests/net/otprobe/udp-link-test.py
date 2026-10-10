#!/usr/bin/env python3
"""Reject build-host library dependencies in the native UDP helper."""
from pathlib import Path
import struct
import sys

b = Path(sys.argv[1]).read_bytes()
assert b[:6] == b'\x7fELF\x01\x02'
off = struct.unpack_from('>I', b, 32)[0]
size, count = struct.unpack_from('>HH', b, 46)
sections = [struct.unpack_from('>10I', b, off + i * size) for i in range(count)]
dyn = next(s for s in sections if s[1] == 6)
strings = sections[dyn[6]]
names = b[strings[4]:strings[4] + strings[5]]
needed = []
for i in range(0, dyn[5], 8):
    tag, value = struct.unpack_from('>II', b, dyn[4] + i)
    if tag == 1:
        needed.append(names[value:].split(b'\0', 1)[0])
assert needed == [b'libsocket.so', b'libnsl.so', b'/usr/lib/libc.so.1'], needed
print('PASS native UDP target library dependencies')
