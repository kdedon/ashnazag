#!/usr/bin/env python3
import struct
import unittest
from adf import ADF


def checksum(block):
    struct.pack_into('>I', block, 20, 0)
    struct.pack_into('>I', block, 20, (-sum(struct.unpack('>128I', block))) & 0xffffffff)


def image(ffs=True):
    disk = bytearray(901120)
    disk[:4] = b'DOS' + bytes([int(ffs)])
    root = bytearray(512)
    for index, value in ((0, 2), (3, 72), (6, 2), (127, 1)):
        struct.pack_into('>I', root, index * 4, value)
    checksum(root)
    disk[880 * 512:881 * 512] = root
    header = bytearray(512)
    for index, value in ((0, 2), (1, 2), (2, 1), (77, 3), (81, 5), (125, 880), (127, 0xfffffffd)):
        struct.pack_into('>I', header, index * 4, value)
    header[432:438] = b'\x05Hello'
    checksum(header)
    disk[1024:1536] = header
    data = bytearray(512)
    if ffs:
        data[:5] = b'world'
    else:
        for index, value in ((0, 8), (1, 2), (2, 1), (3, 5)):
            struct.pack_into('>I', data, index * 4, value)
        data[24:29] = b'world'
        checksum(data)
    disk[1536:2048] = data
    return disk


def modify(disk, block_number, offset, value):
    block = disk[block_number * 512:(block_number + 1) * 512]
    struct.pack_into('>I', block, offset, value)
    checksum(block)
    disk[block_number * 512:(block_number + 1) * 512] = block


class ADFTests(unittest.TestCase):
    def test_ffs_and_ofs(self):
        for ffs in (False, True):
            entries = ADF(image(ffs)).entries()
            self.assertEqual([(e.path, e.data) for e in entries], [('Hello', b'world')])

    def test_checksum(self):
        disk = image()
        disk[1024 + 10] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            ADF(disk).entries()

    def test_cycles_and_bounds(self):
        for offset, value in ((124 * 4, 2), (77 * 4, 90000), (126 * 4, 2)):
            disk = image()
            modify(disk, 2, offset, value)
            with self.assertRaises(ValueError):
                ADF(disk).entries()

    def test_name(self):
        disk = image()
        header = disk[1024:1536]
        header[432:435] = b'\x02..'
        checksum(header)
        disk[1024:1536] = header
        with self.assertRaisesRegex(ValueError, 'unsafe filename'):
            ADF(disk).entries()

    def test_truncated_file(self):
        disk = image()
        modify(disk, 2, 81 * 4, 1000)
        with self.assertRaisesRegex(ValueError, 'file length'):
            ADF(disk).entries()

    def test_unknown_format(self):
        for disk in (b'', b'DOS\x07' + bytes(901116)):
            with self.assertRaises(ValueError):
                ADF(disk)


if __name__ == '__main__':
    unittest.main()
