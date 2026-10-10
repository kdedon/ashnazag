from pathlib import Path
import struct, sys, re
out = Path(sys.argv[1])
root = Path(__file__).resolve().parents[3]
b = (out / 'netinit.elf').read_bytes()
at = struct.unpack_from('>I', b, 32)[0]
size, count = struct.unpack_from('>HH', b, 46)
sections = [struct.unpack_from('>10I', b, at + i * size) for i in range(count)]
relocations = 0
for s in sections:
    if s[1] in (4, 9):
        for i in range(s[5] // s[9]):
            kind = struct.unpack_from('>I', b, s[4] + i * s[9] + 4)[0] & 255
            assert kind in (4, 5, 6), 'absolute resident code reference'
            relocations += 1
symbols = {}
symtab = next(s for s in sections if s[1] == 2)
st = sections[symtab[6]]
strings = b[st[4]:st[4] + st[5]]
for i in range(symtab[5] // symtab[9]):
    name, value = struct.unpack_from('>II', b, symtab[4] + i * symtab[9])
    symbols[strings[name:].split(b'\0', 1)[0].decode()] = value
code = (out / 'netinit.bin').read_bytes()
driver = (out / 'enet.drvr').read_bytes()
assert code[symbols['enet']:symbols['enet_end']] == driver
header = (root / 'kernel/guest/mod/uinter/uinet-image.h').read_text()
assert bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', header)) == driver
assert symbols['previous'] - symbols['resident'] == 6
assert code[symbols['previous'] - 4:symbols['previous']] == b'ANET'
assert code.count(bytes.fromhex('a0bd')) == 2
assert code.startswith(bytes.fromhex('48e7fffe'))
assert bytes.fromhex('4cdf7fff4e75') in code
assert bytes.fromhex('48e77f7e') in code and bytes.fromhex('4cdf7efe4e75') in code
print('PASS resident relocations, driver bytes, header bytes, chain slot, cache flush and registers')
