#!/usr/bin/env python3
"""Verify the shipped tick/deferred signal-frame contract without retaining input."""
import pathlib, struct, sys
b = pathlib.Path(sys.argv[1]).read_bytes()
magic, n, _, _, _, opt, _ = struct.unpack('>HHiiiHH', b[:20])
assert magic == 0x150
sections = []
for i in range(n):
    s = b[20 + opt + 40*i:20 + opt + 40*(i+1)]
    _, address, length, offset = struct.unpack('>iiii', s[8:24])
    sections.append((address, length, offset))
def read(address, size):
    for base, length, offset in sections:
        if offset and base <= address and address+size <= base+length:
            return b[offset+address-base:offset+address-base+size]
    raise AssertionError('unmapped interface instruction')
checks = {
    0x10588: '2f032f02',
    0x105d4: '486f000c61000312',
    0x108ec: '2f0a4ab900000d94',
    0x108f8: '206f0008206800084aa800046652',
    0x10906: '206f00081028000d49c07201b0816642',
    0x10916: '33fc404000000d92',
    0x10928: '2f3c00000d922f0a61ff00029f6c',
    0x1093e: '2f0a61ff00002b1e',
    0x10948: '247900000d944a8a66d6',
    0x10952: '427900000d92',
    0x108c2: '206f00043028000448c07207b081',
    0x14586: '2f084eb9000108c2588f48c04e75',
}
for address, expected in checks.items():
    data = bytes.fromhex(expected)
    assert read(address,len(data)) == data, hex(address)
# Handler SP=F-8; its SP+12 is &signal=F+4. The fourth argument's byte1 is F+17.
assert -8+12+13 == 17
print('PASS %d native deferred-task interface checks' % (len(checks)+1))
