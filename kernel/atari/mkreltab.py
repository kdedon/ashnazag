#!/usr/bin/env python3
# mkreltab.py -- relocation table for moving the linked Falcon kernel.
#
#   mkreltab.py elf out.bin    write the table for elf (linked with -q)
#   mkreltab.py -e out.bin     write an empty table
#   mkreltab.py -c elf         check elf's .reltab against its relocations
#
# A site is a 32-bit word holding an address inside the image: every
# R_68K_32 against a symbol defined in the image, and in the kernel
# symbol table (dlm_ksym) every value of such a symbol plus kh_lo and
# kh_hi.  Sites are byte offsets from the image start, all even.
# Encoding, per site in order: the gap from the previous site in words,
# one byte 1-127, or 0x80 | bits 22-16 then bits 15-0; a 0 byte ends it.
# Then padding to a long, the byte count and 'RTAB', ending at edata.
# Relocations the move would break (absolute 8/16-bit, or PC-relative
# to an absolute symbol) are refused.
import struct, sys

SHN_UNDEF, SHN_ABS = 0, 0xfff1
R_68K_NONE, R_68K_32, R_68K_16, R_68K_8 = 0, 1, 2, 3
LINKER = ('etext', 'edata', 'end')


def die(msg):
    sys.exit('mkreltab: ' + msg)


def load(path):
    img = open(path, 'rb').read()
    if img[:4] != b'\x7fELF' or img[4] != 1 or img[5] != 2:
        die(path + ': not a 32-bit big-endian ELF')
    shoff, = struct.unpack_from('>I', img, 32)
    shentsize, shnum, shstrndx = struct.unpack_from('>HHH', img, 46)
    secs = []
    for i in range(shnum):
        secs.append(struct.unpack_from('>IIIIIIIIII', img, shoff + i * shentsize))
    so = secs[shstrndx][4]

    def name(off, base):
        return img[base + off:img.index(b'\0', base + off)].decode()
    names = [name(s[0], so) for s in secs]
    return img, secs, names


def sites(path):
    img, secs, names = load(path)
    symtab = [i for i, s in enumerate(secs) if s[1] == 2]
    if len(symtab) != 1:
        die(path + ': no symbol table')
    st = secs[symtab[0]]
    strbase = secs[st[6]][4]
    syms = []
    for k in range(st[5] // 16):
        nm, val, size, info, other, shndx = struct.unpack_from('>IIIBBH', img, st[4] + k * 16)
        syms.append((img[strbase + nm:img.index(b'\0', strbase + nm)].decode(), val, shndx, info >> 4))
    sym = {s[0]: s for s in syms if s[3] in (1, 2) and s[2] != SHN_UNDEF}
    lo = 0x1000
    hi = sym['end'][1]

    def inimage(s):
        if s[2] == SHN_UNDEF:
            die('relocation against undefined ' + s[0])
        if s[2] != SHN_ABS:
            return True
        return s[0] in LINKER

    out = set()
    nrel = 0
    for i, s in enumerate(secs):
        if s[1] != 4:
            continue
        tgt = secs[s[7]]
        if not tgt[2] & 2:		# SHF_ALLOC
            continue
        for k in range(s[5] // 12):
            off, info, add = struct.unpack_from('>IIi', img, s[4] + k * 12)
            typ, y = info & 0xff, syms[info >> 8]
            if typ == R_68K_NONE:
                continue
            if typ == R_68K_32:
                if inimage(y):
                    a = off if off >= tgt[3] else tgt[3] + off
                    out.add(a - lo)
                    nrel += 1
            elif typ in (R_68K_16, R_68K_8):
                if inimage(y):
                    die('%s: absolute %d-bit relocation to %s' % (names[i], 16 if typ == 2 else 8, y[0]))
            elif not inimage(y):
                die('%s: PC-relative relocation to absolute %s' % (names[i], y[0]))

    # the kernel symbol table, filled after the link
    data = [s for s in secs if s[2] & 2 and s[1] == 1]
    ks = sym['dlm_ksym'][1]

    def word(va):
        for s in data:
            if s[3] <= va < s[3] + s[5]:
                return struct.unpack_from('>I', img, s[4] + va - s[3])[0]
        die('0x%x is not in the image file' % va)
    nks = 0
    if word(ks) == 0x4b53594d:
        nsym, symoff, stroff = word(ks + 8), word(ks + 12), word(ks + 16)
        out.add(ks + 28 - lo)
        out.add(ks + 32 - lo)
        for k in range(1, nsym):
            e = ks + symoff + k * 16
            nm = word(e)
            b = bytearray()
            while True:
                w = struct.pack('>I', word((ks + stroff + nm + len(b)) & ~3))
                c = w[(ks + stroff + nm + len(b)) & 3]
                if c == 0:
                    break
                b.append(c)
            y = sym.get(b.decode())
            if y is None:
                die('ksym %s is not in the symbol table' % b.decode())
            if inimage(y):
                out.add(e + 4 - lo)
                nks += 1
    s = sorted(out)
    if any(a & 1 or a < 0 or a + 4 > hi - lo for a in s):
        die('odd or out-of-image site')
    if any(b - a < 4 for a, b in zip(s, s[1:])):
        die('overlapping sites')
    return s, nrel, nks, secs, names, img, sym


def encode(s):
    b = bytearray()
    prev = 0
    for a in s:
        g = (a - prev) >> 1
        if g < 0x80:
            b.append(g)
        elif g < 1 << 23:
            b += bytes((0x80 | g >> 16, g >> 8 & 0xff, g & 0xff))
        else:
            die('gap too large')
        prev = a
    b.append(0)
    n = len(b)
    b += bytes(-n & 3) + struct.pack('>I', n) + b'RTAB'
    return bytes(b)


args = sys.argv[1:]
if args[:1] == ['-c']:
    s, nrel, nks, secs, names, img, sym = sites(args[1])
    t = encode(s)
    i = names.index('.reltab')
    have = img[secs[i][4]:secs[i][4] + secs[i][5]]
    if have != t or secs[i][3] + secs[i][5] != sym['edata'][1]:
        die('%s: .reltab does not match its relocations' % args[1])
    print('[OK] reltab: %d sites (%d relocations, %d symbols), %d bytes'
          % (len(s), nrel, nks, len(t)))
elif args[:1] == ['-e']:
    open(args[1], 'wb').write(encode([]))
else:
    s, nrel, nks = sites(args[0])[:3]
    open(args[1], 'wb').write(encode(s))
    print('reltab: %d sites, %d bytes' % (len(s), len(encode(s))))
