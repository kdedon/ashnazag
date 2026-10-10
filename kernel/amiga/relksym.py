#!/usr/bin/env python3
"""Turn a checked symbol block into loader-relocated assembly."""
import pathlib
import struct
import sys

block = pathlib.Path(sys.argv[1]).read_bytes()
u32 = lambda o: struct.unpack_from('>I', block, o)[0]
n, sym, strings = u32(8), u32(12), u32(16)
assert u32(0) == 0x4b53594d and len(block) <= 163840
values = {28: 'stext', 32: 'end'}
for i in range(1, n):
    off = sym + i * 16
    start = strings + u32(off)
    name = block[start:block.index(b'\0', start)].decode('ascii')
    if not name.replace('_', '').replace('.', '').isalnum():
        raise SystemExit('unsupported symbol name ' + name)
    values[off + 4] = name
print('\t.data\n\t.balign 4\n\t.globl dlm_ksym\n\t.type dlm_ksym,@object\n\t.size dlm_ksym,163840\ndlm_ksym:')
i = 0
while i < len(block):
    if i in values:
        print('\t.long ' + values[i])
        i += 4
    else:
        end = min(i + 24, len(block))
        end = min([v for v in values if i < v < end] or [end])
        print('\t.byte ' + ','.join(str(v) for v in block[i:end]))
        i = end
print('\t.space %d\n\t.balign 4' % (163840 - len(block)))
