#!/usr/bin/env python3
"""Model loader section placement and verify every exported address."""
import pathlib
import struct
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'guest'))
from elfrel import Elf

e = Elf(sys.argv[1])
sections = ('.text', '.data', '.bss')
ks = e.syms[e.sym('dlm_ksym')]
assert e.sh[ks['shndx']]['nm'] == '.data'
data = e.byname['.data']
expected = {s['name']: s for s in e.syms if s['name'] and s['shndx'] and s['info'] >> 4 in (1, 2)}
for origins in ((0x100000, 0x300000, 0x500000), (0x40000000, 0x40200000, 0x40400000)):
    base = {e.sh.index(e.byname[name]): value for name, value in zip(sections, origins)}
    image = bytearray(e.b[data['offset']:data['offset'] + data['size']])
    r = e.byname['.rela.data']
    special = {'etext': origins[0] + e.byname['.text']['size'],
               'edata': origins[1] + data['size'],
               'end': origins[2] + e.byname['.bss']['size']}
    def address(s):
        if s['shndx'] == 0: return special[s['name']]
        return base.get(s['shndx'], 0) + s['value']
    for pos in range(r['offset'], r['offset'] + r['size'], 12):
        offset, info, addend = struct.unpack_from('>III', e.b, pos)
        if not ks['value'] <= offset < ks['value'] + ks['size']: continue
        assert info & 255 == 1
        struct.pack_into('>I', image, offset, (address(e.syms[info >> 8]) + addend) & 0xffffffff)
    block = image[ks['value']:ks['value'] + ks['size']]
    u32 = lambda p: struct.unpack_from('>I', block, p)[0]
    assert u32(28) == address(expected['stext']) and u32(32) == special['end']
    count, symbols, strings = u32(8), u32(12), u32(16)
    for i in range(1, count):
        off = symbols + i * 16
        p = strings + u32(off)
        name = block[p:block.index(0, p)].decode()
        want = special[name] if name in special else address(expected[name])
        assert u32(off + 4) == want, name
    print('PASS %d exports at section origins %s' % (count - 1, ','.join(hex(n) for n in origins)))
