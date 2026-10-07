#!/usr/bin/env python3
"""run.py -- run the amilib/opci harness on an emulated 68040 (unicorn).

    run.py image.elf image.bin [openpci.library]

The harness program (hmain.c) is linked flat at 0x10000.  Memory:
    0x00000000-0x00ffffff  RAM (heap 0x200000-0xc00000, stack below
                           0xf00000, the library file at 0xc00000)
    0x01000000             console byte port, 0x01000004 halt port
    0x40000000             a Prometheus board (Matay, 0xad47/1): PCI
                           config space at +0xf0000 + slot<<13 + fn<<8,
                           bytes as on the PCI bus (address invariant);
                           the rest of the board is plain memory.
One PASS/FAIL line per check; exit 1 on any FAIL.  Without the library
the openpci tests are skipped (it is not ours to ship).
"""

import struct
import subprocess
import sys

from unicorn import Uc, UcError, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, \
    UC_HOOK_MEM_UNMAPPED, UC_HOOK_INTR, UC_PROT_ALL
from unicorn.m68k_const import UC_CPU_M68K_M68040, UC_M68K_REG_PC, \
    UC_M68K_REG_SR, UC_M68K_REG_A7

RAM = 0x01000000
PORT = 0x01000000
LIBAT = 0x00c00000
HUNKAT = 0x00e00000
PROM = 0x40000000
PROMSIZE = 0x20000000
CFG = 0xf0000

fails = 0


def check(name, ok, detail=''):
    global fails
    print('[%s] %s%s' % ('PASS' if ok else 'FAIL', name,
                         (': ' + detail) if detail and not ok else ''))
    if not ok:
        fails += 1


class Card:
    """A PCI function: 256 bytes of config space, little endian."""

    def __init__(self, vendor, device, cls, bars):
        self.cfg = bytearray(256)
        struct.pack_into('<HHHH', self.cfg, 0, vendor, device, 0, 0x0200)
        struct.pack_into('<I', self.cfg, 8, (cls << 8) | 1)
        self.cfg[0x3d] = 1                          # INTA
        self.bars = bars                            # [(size, flags)] * n
        for i, (size, flags) in enumerate(bars):
            struct.pack_into('<I', self.cfg, 0x10 + 4 * i, flags)

    def write(self, reg, data):
        for k, b in enumerate(data):
            r = reg + k
            if r < 4 or 8 <= r < 0x0c or r == 0x0e or r == 0x3d:
                continue                        # read-only
            self.cfg[r] = b
        for i, (size, flags) in enumerate(self.bars):
            o = 0x10 + 4 * i
            if o + 4 > reg and o < reg + len(data):
                v = struct.unpack_from('<I', self.cfg, o)[0]
                v = (v & ~(size - 1) & 0xffffffff) | flags
                struct.pack_into('<I', self.cfg, o, v)
        for o in range(0x18 if self.bars else 0x10, 0x28, 4):
            if o >= 0x10 + 4 * len(self.bars):
                struct.pack_into('<I', self.cfg, o, 0)


class Prometheus:
    def __init__(self, cards):
        self.cards = cards                      # {(slot, fn): Card}
        self.mem = {}
        self.other = []

    def card(self, off):
        o = off - CFG
        if 0 <= o < 4 * 0x2000:
            return self.cards.get((o >> 13, (o >> 8) & 7)), o & 0xff
        return None, None

    def read(self, uc, off, size, ud):
        c, reg = self.card(off)
        if reg is not None:
            if c is None:
                return (1 << (8 * size)) - 1
            return int.from_bytes(bytes(c.cfg[reg:reg + size]), 'big')
        if len(self.other) < 40:
            self.other.append(('r', off, size))
        return int.from_bytes(bytes(self.mem.get(off + k, 0)
                                    for k in range(size)), 'big')

    def write(self, uc, off, size, value, ud):
        data = (value & ((1 << (8 * size)) - 1)).to_bytes(size, 'big')
        c, reg = self.card(off)
        if reg is not None:
            if c is not None:
                c.write(reg, data)
            return
        if len(self.other) < 40:
            self.other.append(('w', off, size, value))
        for k, b in enumerate(data):
            self.mem[off + k] = b


def syms(elf):
    out = subprocess.run(['m68k-linux-gnu-nm', elf], capture_output=True,
                         text=True, check=True).stdout
    s = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            s[p[2]] = int(p[0], 16)
    return s


def run(image, sym, mode, lib=None, boards=(), hunks=(), model=None):
    uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    uc.ctl_set_cpu_model(UC_CPU_M68K_M68040)
    uc.mem_map(0, RAM, UC_PROT_ALL)
    uc.mem_write(0x10000, image)
    cons = bytearray()
    halt = []

    def port_r(uc, off, size, ud):
        return 0

    def port_w(uc, off, size, value, ud):
        if off == 0:
            cons.append(value & 0xff)
        elif off == 4:
            halt.append(value)
            uc.emu_stop()

    uc.mmio_map(PORT, 0x1000, port_r, None, port_w, None)
    if model:
        uc.mmio_map(PROM, PROMSIZE, model.read, None, model.write, None)

    def w32(name, v):
        uc.mem_write(sym[name], struct.pack('>I', v & 0xffffffff))

    w32('h_mode', mode)
    if lib:
        uc.mem_write(LIBAT, lib)
        w32('h_lib', LIBAT)
        w32('h_liblen', len(lib))
    zbsize = struct.unpack('>I', uc.mem_read(sym['h_zbsize'], 4))[0]
    for i, b in enumerate(boards):
        z = struct.pack('>IIHBBBxIH', *b)
        uc.mem_write(sym['h_boards'] + zbsize * i, z + b'\0' * (zbsize - 20))
    w32('h_nboards', len(boards))
    at = HUNKAT
    for i, h in enumerate(hunks):
        uc.mem_write(at, h)
        uc.mem_write(sym['h_hunk'] + 4 * i, struct.pack('>I', at))
        uc.mem_write(sym['h_hunklen'] + 4 * i, struct.pack('>I', len(h)))
        at += (len(h) + 15) & ~15
    uc.mem_write(sym['h_hunk'] + 4 * len(hunks), b'\0' * 4)

    def bad(uc, access, addr, size, value, ud):
        print('  unmapped access %x at pc %x' % (addr,
              uc.reg_read(UC_M68K_REG_PC)))
        return False

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, bad)

    def intr(uc, intno, ud):
        # unicorn stops on every CPU exception; rte (QEMU's EXCP_RTE) is
        # one.  Do it here: format 0 and 2 frames, as a 68040 would.
        sp = uc.reg_read(UC_M68K_REG_A7)
        if intno != 0x100:
            print('  CPU exception %x at pc %x' % (intno,
                  uc.reg_read(UC_M68K_REG_PC)))
            uc.emu_stop()
            return
        sr, pc, fv = struct.unpack('>HIH', uc.mem_read(sp, 8))
        size = {0: 8, 2: 12}.get(fv >> 12)
        if size is None:
            print('  rte with frame format %x' % (fv >> 12))
            uc.emu_stop()
            return
        uc.reg_write(UC_M68K_REG_A7, sp + size)
        uc.reg_write(UC_M68K_REG_SR, sr)
        uc.reg_write(UC_M68K_REG_PC, pc)

    uc.hook_add(UC_HOOK_INTR, intr)
    uc.reg_write(UC_M68K_REG_SR, 0x2700)        # supervisor, as the kernel
    uc.reg_write(UC_M68K_REG_A7, 0x00f00000)
    err = None
    try:
        uc.emu_start(0x10000, 0xffffffff, count=400_000_000)
    except UcError as e:
        pc = uc.reg_read(UC_M68K_REG_PC)
        err = '%s at pc %x' % (e, pc)
        if lib:                                 # where in the library
            ram = bytes(uc.mem_read(0x200000, 0xa00000))
            k = ram.find(bytes.fromhex('70ff4e754afc'))
            if k >= 0 and 0x200000 + k <= pc:
                err += ' (library +%x)' % (pc - 0x200000 - k)
    n = struct.unpack('>I', uc.mem_read(sym['h_nres'], 4))[0]
    res = list(struct.unpack('>%di' % 64, uc.mem_read(sym['h_res'], 256)))
    text = cons.decode('latin-1')
    for line in text.splitlines():
        print('  | ' + line)
    if err or not halt:
        check('mode %d ran to the end' % mode, False,
              err or 'no halt (instruction limit)')
    return uc, res[:n], text


def cstr(uc, a):
    b = bytes(uc.mem_read(a, 128))
    return b.split(b'\0')[0].decode('latin-1')


def hunkfile(hunks, relocs, short=False, header_names=False, cut=0):
    """hunks: [(type, bytes)]; relocs: {hunk: {target: [offsets]}}"""
    w = lambda *v: b''.join(struct.pack('>I', x & 0xffffffff) for x in v)
    f = w(0x3f3) + (w(1, 0x74657374) if header_names else b'') + w(0)
    f += w(len(hunks), 0, len(hunks) - 1)
    f += b''.join(w((len(d) + 3) // 4) for t, d in hunks)
    for i, (t, d) in enumerate(hunks):
        d = d + b'\0' * (-len(d) % 4)
        f += w(t, len(d) // 4) + (d if t != 0x3eb else b'')
        if i in relocs:
            if short:
                h = b''
                for tgt, offs in relocs[i].items():
                    h += struct.pack('>HH', len(offs), tgt)
                    h += b''.join(struct.pack('>H', o) for o in offs)
                h += b'\0\0'
                h += b'\0' * (-len(h) % 4)
                f += w(0x3fc) + h
            else:
                f += w(0x3ec)
                for tgt, offs in relocs[i].items():
                    f += w(len(offs), tgt, *offs)
                f += w(0)
        f += w(0x3f0, 1, 0x61626364, 0, 0) + w(0x3f2)
    return f[:len(f) - cut] if cut else f


def main():
    elf, binf = sys.argv[1], sys.argv[2]
    lib = open(sys.argv[3], 'rb').read() if len(sys.argv) > 3 else None
    image = open(binf, 'rb').read()
    sym = syms(elf)

    # ---- mode 2: LoadSeg
    code = struct.pack('>III', 0x11111111, 4, 0x22222222)
    data = struct.pack('>II', 0x33333333, 0x44444444)
    good = hunkfile([(0x3e9, code), (0x3ea, data), (0x3eb, b'\0' * 16)],
                    {0: {1: [4]}})
    short = hunkfile([(0x3e9, code), (0x3ea, data)], {0: {1: [4]}},
                     short=True)
    files = [good, short, good[:-20],
             hunkfile([(0x3e9, code)], {0: {0: [12]}}),
             hunkfile([(0x3e9, code)], {}, header_names=True),
             hunkfile([(0x3e9, code)], {0: {3: [0]}})]
    uc, r, _ = run(image, sym, 2, hunks=files)
    for i, name in enumerate(['three hunks, RELOC32', 'RELOC32SHORT']):
        e, s, l0, l1, h1 = r[5 * i:5 * i + 5]
        check('LoadSeg %s' % name, e == 0 and s and l0 == 0x11111111 and
              l1 == (h1 + 4 + 4) & 0xffffffff,
              'err %d seg %x %x %x hunk1 %x' % (e, s, l0, l1, h1))
    for i, name in enumerate(['truncated file', 'relocation past the hunk',
                              'resident library names', 'relocation to a '
                              'missing hunk']):
        e = r[5 * (i + 2)]
        check('LoadSeg refuses a %s' % name, e == 1, 'err %d' % e)
    check('LoadSeg frees everything', r[5 * len(files)] == 0,
          '%d bytes' % r[5 * len(files)])

    # ---- mode 3: exec and utility
    uc, r, _ = run(image, sym, 3)
    check('exec comes up', r and r[0] == 0)
    if len(r) >= 12:
        s = cstr(uc, r[1] & 0xffffffff)
        check('RawDoFmt', s == 'lib 42 beef -2|   ab|', repr(s))
        check('OpenLibrary utility.library', r[2] == 1)
        check('GetTagData across TAG_IGNORE', r[3] == 22 and r[4] == 99,
              '%d %d' % (r[3], r[4]))
        check('Stricmp', r[5] == 0)
        p = 0x12345678 * 0x9abcdef0
        check('UMult64', (r[6] & 0xffffffff, r[7] & 0xffffffff) ==
              (p >> 32, p & 0xffffffff))
        check('Enqueue by priority, FindName', r[8] == 1 and r[9] == 1)
        check('OpenLibrary of a library we lack fails', r[10] == 1)
        check('exec frees everything', r[11] == 0, '%d bytes' % r[11])

    if lib is None:
        print('[SKIP] openpci.library tests: no library given')
        return

    # ---- mode 1: no bridge
    uc, r, text = run(image, sym, 1, lib=lib)
    check('no bridge: opci_init fails with ENXIO', r and r[0] == 6,
          str(r[:2]))
    check('no bridge: nothing left allocated', len(r) > 1 and r[1] == 0,
          str(r[:2]))

    # ---- mode 0: a Prometheus with two cards
    model = Prometheus({
        (0, 0): Card(0x10ec, 0x8139, 0x020000, [(256, 1), (256, 0)]),
        (2, 0): Card(0x5333, 0x8811, 0x030000, [(0x400000, 8)]),
    })
    prom = (PROM, PROMSIZE, 0xad47, 1, 0x80 | 0x20, 0x30, 0x12345678, 0)
    uc, r, text = run(image, sym, 0, lib=lib, boards=[prom], model=model)
    for o in model.other:
        print('  board access outside config space:', o)
    nl = [l for l in text.splitlines() if 'not provided' in l]
    check('no LVO the library needs is missing', not nl, '; '.join(nl))
    if len(r) < 28:
        check('opci run complete', False, '%d results' % len(r))
        return
    check('opci_init', r[0] == 0, 'error %d' % r[0])
    check('bridge reported as Prometheus', r[1] & 4, '%x' % r[1])
    check('a device found', r[2] == 1)
    check('config word reads (vendor, device)', (r[3], r[4]) ==
          (0x10ec, 0x8139), '%x %x' % (r[3], r[4]))
    check('config long read', r[5] & 0xffffffff == 0x813910ec,
          '%x' % (r[5] & 0xffffffff))
    check('attributes vendor and device', (r[6], r[7]) == (0x10ec, 0x8139),
          '%x %x' % (r[6], r[7]))
    bar0 = r[8] & 0xffffffff
    check('BAR 0 mapped inside the board', PROM <= bar0 < PROM + PROMSIZE,
          '%x size %x cfg %x' % (bar0, r[9] & 0xffffffff,
                                 r[10] & 0xffffffff))
    check('decoding enabled in the command register', r[11] & 3,
          '%x' % r[11])
    check('obtain, busy, release, obtain', (r[12], r[13], r[14]) ==
          (0, 16, 0), str(r[12:15]))
    check('find by vendor and device', r[15] == 1 and r[16] == 1)
    check('the second card', r[17] == 1 and r[18] == 0x5333,
          '%x' % r[18])
    check('config byte write and read back', r[19] == 0x5a, '%x' % r[19])
    check('interrupt server added', r[20] == 0 and r[21] != 0,
          'error %d chains %x' % (r[20], r[21]))
    check('a bridge interrupt reaches the device handler', r[23] == 1,
          '%d calls' % r[23])
    check('no calls after opci_unintr', r[24] == 1, '%d calls' % r[24])
    check('opci_fini', r[25] == 0, 'error %d' % r[25])
    check('nothing left allocated after unload', r[26] == 0,
          '%d bytes' % r[26])


if __name__ == '__main__':
    main()
    sys.exit(1 if fails else 0)
