#!/usr/bin/env python3
"""Give an AHDI disk image a small GEMDOS partition holding \\AUTO\\AXBOOT.PRG.

  mkgemboot.py IMAGE PRG

EmuTOS never runs a hard disk's root sector; it runs AUTO programs from
the boot drive.  The partition (FAT16, last 3 MB of the disk) goes in the
first ICD slot, leaving the four primary entries as they are.  A bootable
root sector stays bootable.
"""
import struct, sys

SEC = 512
NSEC = 3 * 2048
ICD = 0x156
ROOTENT = 512


def wsum(s):
    return sum(struct.unpack('>256H', bytes(s))) & 0xFFFF


def die(msg):
    print('[FAIL] ' + msg)
    sys.exit(1)


def fat16(prg):
    """A FAT16 image of NSEC sectors: \\AUTO\\AXBOOT.PRG."""
    fatsec = (NSEC * 2 + SEC - 1) // SEC
    rootsec = ROOTENT * 32 // SEC
    data0 = 1 + 2 * fatsec + rootsec
    if NSEC - data0 <= 4085:
        die('GEMDOS partition too small for FAT16')
    fs = bytearray(NSEC * SEC)
    bs = fs[0:SEC]
    bs[0:3] = b'\xeb\x3c\x90'
    bs[3:11] = b'AXBOOT  '
    struct.pack_into('<HBHBHHBHHHII', bs, 11, SEC, 1, 1, 2, ROOTENT,
                     NSEC, 0xF8, fatsec, 32, 64, 0, 0)
    bs[0x26] = 0x29
    bs[0x2B:0x3E] = b'AXBOOT     FAT16   '
    fs[0:SEC] = bs
    # cluster 2: AUTO directory, 3 on: the program
    ncl = (len(prg) + SEC - 1) // SEC
    fat = [0xFFF8, 0xFFFF, 0xFFFF] + list(range(4, 3 + ncl)) + [0xFFFF]
    fatb = struct.pack('<%dH' % len(fat), *fat)
    for i in range(2):
        o = (1 + i * fatsec) * SEC
        fs[o:o + len(fatb)] = fatb

    def ent(name, attr, cl, size):
        return struct.pack('<11sB10sHHHI', name, attr, bytes(10), 0, 0x5521, cl, size)
    o = (1 + 2 * fatsec) * SEC
    fs[o:o + 32] = ent(b'AXBOOT     ', 0x08, 0, 0)
    fs[o + 32:o + 64] = ent(b'AUTO       ', 0x10, 2, 0)
    o = data0 * SEC
    fs[o:o + 96] = ent(b'.          ', 0x10, 2, 0) + ent(b'..         ', 0x10, 0, 0) \
        + ent(b'AXBOOT  PRG', 0x20, 3, len(prg))
    o += SEC
    fs[o:o + len(prg)] = prg
    return fs


def main():
    img, prgf = sys.argv[1:3]
    prg = open(prgf, 'rb').read()
    with open(img, 'r+b') as f:
        rs = bytearray(f.read(SEC))
        old = wsum(rs)
        total, = struct.unpack_from('>I', rs, 0x1C2)
        st = total - NSEC
        for i in range(4):
            flg, pid, pst, n = struct.unpack_from('>B3sII', rs, 0x1C6 + 12 * i)
            if flg & 1 and pst + n > st:
                die('no free 3 MB at the end of the disk')
        if rs[ICD:ICD + 12].count(0) != 12 and rs[ICD + 1:ICD + 4] != b'GEM':
            die('ICD slot in use')
        struct.pack_into('>B3sII', rs, ICD, 1, b'GEM', st, NSEC)
        if old == 0x1234:
            w, = struct.unpack_from('>H', rs, 0x1FE)
            struct.pack_into('>H', rs, 0x1FE, (w + 0x1234 - wsum(rs)) & 0xFFFF)
        f.seek(0)
        f.write(rs)
        f.seek(st * SEC)
        f.write(fat16(prg))
    print('GEM %d+%d: \\AUTO\\AXBOOT.PRG, %d bytes' % (st, NSEC, len(prg)))


main()
