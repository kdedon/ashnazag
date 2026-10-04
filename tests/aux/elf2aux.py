#!/usr/bin/env python3
"""elf2aux.py -- an ELF executable with .text, .data and .bss as an A/UX
static COFF program: magic 0x150, a.out 0413, three sections.  Text
and data sit at file offsets congruent to their addresses mod 4 KB, so
the kernel maps them instead of copying; link text at 0x10a8.

    python3 elf2aux.py in.elf out
"""
import struct
import sys


def sections(d):
    shoff, = struct.unpack('>I', d[32:36])
    shentsize, shnum, shstrndx = struct.unpack('>HHH', d[46:52])
    sh = [struct.unpack('>IIIIIIIIII', d[shoff + i * shentsize:shoff + i * shentsize + 40])
          for i in range(shnum)]
    stroff = sh[shstrndx][4]
    out = {}
    for s in sh:
        n = d[stroff + s[0]:d.index(b'\0', stroff + s[0])].decode()
        out[n] = s
    return out


def main():
    d = open(sys.argv[1], 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 2:
        sys.exit('elf2aux: not a 32-bit big-endian ELF file')
    entry, = struct.unpack('>I', d[24:28])
    sh = sections(d)
    for n in ('.text', '.data', '.bss'):
        if n not in sh:
            sys.exit('elf2aux: no %s section' % n)
    text = d[sh['.text'][4]:sh['.text'][4] + sh['.text'][5]]
    data = d[sh['.data'][4]:sh['.data'][4] + sh['.data'][5]]
    tva, dva, bva = sh['.text'][3], sh['.data'][3], sh['.bss'][3]
    bsize = sh['.bss'][5]
    if bva != dva + len(data):
        sys.exit('elf2aux: .bss must follow .data')
    toff = 20 + 28 + 3 * 40
    doff = toff + len(text)
    doff += (dva - doff) & 0xfff
    if (tva - toff) & 0xfff:
        sys.exit('elf2aux: text address must be 0x%x mod 4 KB' % toff)
    o = struct.pack('>HHiiiHH', 0x150, 3, 0, 0, 0, 28, 0x20f)
    o += struct.pack('>hhiiiiii', 0o413, 0, len(text), len(data), bsize, entry, tva, dva)

    def scn(name, va, size, off, flags):
        return struct.pack('>8siiiiiiHHi', name, va, va, size, off, 0, 0, 0, 0, flags)
    o += scn(b'.text', tva, len(text), toff, 0x20)
    o += scn(b'.data', dva, len(data), doff, 0x40)
    o += scn(b'.bss', bva, bsize, 0, 0x80)
    o += text
    o += bytes(doff - len(o))
    open(sys.argv[2], 'wb').write(o + data)


main()
