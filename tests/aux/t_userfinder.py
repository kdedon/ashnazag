#!/usr/bin/env python3
"""Check both Finder menu formats and unknown-resource rejection."""
import importlib.util
from pathlib import Path
import struct
import unittest

path = Path(__file__).resolve().parents[2] / 'images/macenv/userfinder.py'
spec = importlib.util.spec_from_file_location('userfinder', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Menus(unittest.TestCase):
    def menu(self, title, fixed):
        header = b'\0\1\0\3' + b'\0' * (title - 4) + b'\x07Special'
        def item(cmd, name):
            return cmd + b'\0' * (fixed - 4) + bytes([len(name)]) + name + (b'\0' if len(name) % 2 == 0 else b'')
        return header + item(b'rest', b'Restart') + item(b'shut', b'Shut Down')

    def test_formats(self):
        for title, fixed in ((12, 8), (16, 12)):
            original = self.menu(title, fixed)
            changed = module.logout_menu(original, title, fixed)
            self.assertNotIn(b'rest', changed)
            self.assertNotIn(b'Shut Down', changed)
            self.assertIn(b'shut', changed)
            self.assertIn(b'\x07Log Out', changed)
            self.assertEqual(struct.unpack('>H', changed[2:4])[0], 2)
            self.assertEqual(module.logout_menu(changed, title, fixed), changed)

    def test_dynamic_labels(self):
        strings = [b'x'] * 56 + [b'Shut Down', b'Shut Down ']
        original = struct.pack('>H', len(strings)) + b''.join(bytes([len(v)]) + v for v in strings)
        changed = module.logout_strings(original)
        self.assertNotIn(b'Shut Down', changed)
        self.assertIn(b'Log Out', changed)
        self.assertEqual(module.logout_strings(changed), changed)
        with self.assertRaises(ValueError):
            module.logout_strings(original.replace(b'Shut Down', b'Power Off'))


    def test_unknown_title(self):
        with self.assertRaises(ValueError):
            module.logout_menu(self.menu(16, 12).replace(b'Special', b'Unknown'), 16, 12)

    def test_missing_shutdown(self):
        with self.assertRaises(ValueError):
            module.logout_menu(self.menu(16, 12).replace(b'shut', b'xxxx'), 16, 12)

    def test_truncated(self):
        with self.assertRaises(ValueError):
            module.logout_menu(self.menu(16, 12)[:-1], 16, 12)


if __name__ == '__main__':
    unittest.main()
