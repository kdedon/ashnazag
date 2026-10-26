#!/usr/bin/env python3
# patch_kptest.py IMAGE [SYM]: the kernel-fault handler (krnxmemflt) tests the
# faulting address with ptest(), which walks the user root.  A user mapping in
# the kernel's 0x40000000 section (the Mac ROM at 0x40800000) then hides the
# kernel's own invalid entry and the fault fails.  Retarget that call to SYM
# (default kptest), which walks the supervisor root.
import struct
import sys

IMG = sys.argv[1]
NEW = sys.argv[2] if len(sys.argv) > 2 else "kptest"
RELOC, OLD = 0x0005B15C, "ptest"


def u16(b, o): return struct.unpack(">H", b[o:o+2])[0]
def u32(b, o): return struct.unpack(">I", b[o:o+4])[0]


buf = bytearray(open(IMG, "rb").read())
shoff, shent, shnum, shstrndx = u32(buf, 32), u16(buf, 46), u16(buf, 48), u16(buf, 50)
sh = []
for i in range(shnum):
    o = shoff + i * shent
    sh.append((u32(buf, o), u32(buf, o + 16), u32(buf, o + 20), u32(buf, o + 24), u32(buf, o + 36)))
shstr = sh[shstrndx][1]


def cstr(o): return buf[o:buf.index(b"\0", o)].decode()


rela = [s for s in sh if cstr(shstr + s[0]) == ".rela.text"][0]
symtab = sh[rela[3]]
strtab = sh[symtab[3]]


def symname(i): return cstr(strtab[1] + u32(buf, symtab[1] + i * symtab[4]))


for i in range(rela[2] // rela[4]):
    o = rela[1] + i * rela[4]
    if u32(buf, o) != RELOC:
        continue
    info = u32(buf, o + 4)
    cur = symname(info >> 8)
    if cur == NEW:
        sys.exit(0)
    if cur != OLD:
        sys.exit("patch_kptest: reloc @0x%x names %r, expected %r" % (RELOC, cur, OLD))
    for j in range(symtab[2] // symtab[4]):
        if symname(j) == NEW:
            struct.pack_into(">I", buf, o + 4, (j << 8) | (info & 0xFF))
            open(IMG, "wb").write(buf)
            print("  [ok] krnxmemflt: ptest -> %s" % NEW)
            sys.exit(0)
    sys.exit("patch_kptest: %s not defined" % NEW)
sys.exit("patch_kptest: no reloc @0x%x" % RELOC)
