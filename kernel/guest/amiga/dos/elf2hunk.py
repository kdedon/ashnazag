#!/usr/bin/env python3
"""Convert a single-section m68k ELF object to a relocatable Amiga load file."""
import argparse
import struct
from pathlib import Path


def convert(raw):
    if raw[:7] != b'\x7fELF\x01\x02\x01':
        raise ValueError('expected big-endian ELF32')
    h = struct.unpack_from('>HHIIIIIHHHHHH', raw, 16)
    if h[0:2] != (1, 4):
        raise ValueError('expected relocatable m68k object')
    shoff, shsize, shnum, shstr = h[5], h[10], h[11], h[12]
    if shsize != 40:
        raise ValueError('invalid ELF section header size')
    sections = [struct.unpack_from('>10I', raw, shoff+i*40) for i in range(shnum)]
    names = raw[sections[shstr][4]:sections[shstr][4]+sections[shstr][5]]
    name = lambda s: names[s[0]:].split(b'\0', 1)[0]
    indexes = [i for i, s in enumerate(sections) if name(s) == b'.text']
    if len(indexes) != 1:
        raise ValueError('expected one text section')
    text_index = indexes[0]
    for i, s in enumerate(sections):
        if s[2] & 2 and s[5] and i != text_index:
            raise ValueError('only the text section may contain allocated data')
    s = sections[text_index]
    code = bytearray(raw[s[4]:s[4]+s[5]])
    relocations = []
    for r in sections:
        if r[1] not in (4, 9) or r[7] != text_index:
            continue
        if r[1] != 4 or r[9] != 12:
            raise ValueError('expected ELF RELA relocations')
        syms = sections[r[6]]
        if syms[1] != 2 or syms[9] != 16:
            raise ValueError('invalid symbol table')
        for at in range(r[4], r[4]+r[5], 12):
            offset, info, addend = struct.unpack_from('>IIi', raw, at)
            sym_index, kind = info >> 8, info & 255
            if sym_index >= syms[5]//16:
                raise ValueError('invalid relocation symbol')
            _, value, _, _, _, section = struct.unpack_from('>IIIBBH', raw, syms[4]+sym_index*16)
            if section != text_index or kind not in (1, 4):
                raise ValueError(f'unsupported relocation: kind={kind}, section={section}')
            if offset+4 > len(code) or offset & 1:
                raise ValueError('invalid relocation offset')
            target = value+addend
            if not 0 <= target <= len(code):
                raise ValueError('relocation target outside text')
            if kind == 4:
                struct.pack_into('>i', code, offset, target-offset)
            else:
                struct.pack_into('>I', code, offset, target)
                relocations.append(offset)
    if len(relocations) != len(set(relocations)):
        raise ValueError('duplicate relocation')
    code.extend(b'\0' * (-len(code) % 4))
    pack = lambda values: struct.pack('>'+'I'*len(values), *values)
    out = pack([1011, 0, 1, 0, 0, len(code)//4, 1001, len(code)//4])+code
    if relocations:
        out += pack([1004, len(relocations), 0]+sorted(relocations)+[0])
    return out+pack([1010])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    a.output.write_bytes(convert(a.input.read_bytes()))


if __name__ == '__main__':
    main()
