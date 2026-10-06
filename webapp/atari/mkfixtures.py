#!/usr/bin/env python3
"""mkfixtures.py BOOTSEC LOADER OUTDIR -- write the test fixtures.

A 512 MiB disk (4 MiB boot, 128 MiB root, 64 MiB swap, 312 MiB /home) gets
the boot path from instboot.py; rootsec.bin and axb.bin are read back from
it.  kernel.elf is a minimal m68k ELF with one loadable segment.
"""
import os, struct, subprocess, sys, tempfile

SEC = 512
HERE = os.path.dirname(os.path.abspath(__file__))
INST = os.path.join(HERE, '..', '..', 'kernel', 'atari', 'instboot.py')


def kernel():
    k = bytearray(SEC)
    k[:16] = b'\x7fELF\x01\x02\x01' + bytes(9)
    struct.pack_into('>HHIIIIIHHHHHH', k, 16, 2, 4, 1, 0x100000, 0x34, 0,
                     0, 0x34, 0x20, 1, 0, 0, 0)
    struct.pack_into('>IIIIIIII', k, 0x34, 1, 0x100, 0x100000, 0x100000,
                     0x20, 0x200, 5, 4)
    k[0x100:0x120] = b'N' * 32
    return bytes(k)


def rootsector(disk, parts):
    rs = bytearray(SEC)
    struct.pack_into('>I', rs, 0x1C2, disk * 2048)
    st = 64
    for i, (pid, mib) in enumerate(parts):
        n = mib * 2048
        struct.pack_into('>B3sII', rs, 0x1C6 + 12 * i, 1, pid, st, n)
        st += n
    return rs


def main(bootsec, loader, out):
    os.makedirs(out, exist_ok=True)
    kern = kernel()
    with open(os.path.join(out, 'kernel.elf'), 'wb') as f:
        f.write(kern)
    with tempfile.TemporaryDirectory() as d:
        img = os.path.join(d, 'disk.img')
        with open(img, 'wb') as f:
            f.write(rootsector(512, [(b'AXB', 4), (b'AXR', 128),
                                     (b'AXS', 64), (b'AXU', 312)]))
            f.truncate(512 << 20)
        kf = os.path.join(d, 'kernel.elf')
        with open(kf, 'wb') as f:
            f.write(kern)
        subprocess.run([sys.executable, INST, img, bootsec, loader, kf,
                        'root=c0d0s1', '3'], check=True,
                       stdout=subprocess.DEVNULL)
        with open(img, 'rb') as f:
            rs = f.read(SEC)
            f.seek(64 * SEC)
            axb = f.read(17 * SEC)
    with open(os.path.join(out, 'rootsec.bin'), 'wb') as f:
        f.write(rs)
    with open(os.path.join(out, 'axb.bin'), 'wb') as f:
        f.write(axb)


if len(sys.argv) != 4:
    sys.exit(__doc__)
main(*sys.argv[1:])
