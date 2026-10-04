#!/usr/bin/env python3
"""Host checks for ROM validation and CD path selection."""
import struct
import unittest
from unittest.mock import patch

import media


class MediaTests(unittest.TestCase):
    def rom(self):
        data = bytearray(0x80000)
        struct.pack_into('>II', data, 0, 0x11144ef9, 0xf800d2)
        struct.pack_into('>I', data, len(data) - 4, 0xffffffff - 0x11144ef9 - 0xf800d2)
        return data

    def test_checksum_and_identity(self):
        info = media.rom_info(self.rom())
        self.assertFalse(info['a4000_32'])
        self.assertEqual(info['reset_pc'], '0xf800d2')

    def test_corrupt_and_truncated(self):
        data = self.rom()
        data[128] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            media.rom_info(data)
        with self.assertRaisesRegex(ValueError, '512 KiB'):
            media.rom_info(data[:-1])

    def test_paths(self):
        listing = b'/ROM/kicka4000.rom;1\n/ADF/Install3.2.adf;1\n/ROM/../bad.rom;1\n/ROM.info;1\n/Other/\xa7;1\n'
        with patch.object(media, 'iso', return_value=listing):
            paths = media.entries('unused.iso')
        self.assertEqual([name for _, name in paths], ['ROM/kicka4000.rom', 'ADF/Install3.2.adf'])

    def test_missing_or_duplicate(self):
        for listing in (b'/ADF/Install3.2.adf;1\n', b'/ROM/kicka4000.rom;1\n/ROM/kicka4000.rom;1\n'):
            with patch.object(media, 'iso', return_value=listing):
                with self.assertRaises(ValueError):
                    media.entries('unused.iso')


if __name__ == '__main__':
    unittest.main()
