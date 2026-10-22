import os
import struct
import tempfile
import unittest
from lisavol import Vol, install

IMAGE = os.path.join(os.environ.get('LISAMEDIA') or
    os.path.join(os.path.dirname(os.path.abspath(__file__)), '../media/lisa/imported'), 'profile.dc42')


def read(*p):
    with open(os.path.join(*p), 'rb') as f:
        return f.read()


def tag(fid, page, own, nxt=0xffffff, prv=0xffffff):
    return struct.pack('>IhHI', 0, fid, 0, own)[:8] + own.to_bytes(3, 'big') + b'\0' + \
        struct.pack('>H', page) + nxt.to_bytes(3, 'big') + prv.to_bytes(3, 'big')


def entry(name, s, mod, leof):
    e = bytearray(64)
    e[0] = 0x24
    e[3:3 + len(name)] = name
    e[36] = 3
    struct.pack_into('>HIIII', e, 38, s, mod, mod, leof, (leof + 511) // 512 * 512)
    return bytes(e)


class Install(unittest.TestCase):
    def fixture(self):
        """Raw image: boot block, a catalog page with a stale and a current
        entry, and file 5 "tmp/a.text" of 700 bytes in two pages."""
        blk = []
        blk.append(tag(-0x5556, 0, 0) + b'B' * 512)
        cat = entry(b'tmp/a.text', 5, 1, 600) + entry(b'tmp/a.text', 5, 2, 700)
        blk.append(tag(4, 0, 1) + cat.ljust(512, b'\0'))
        h = bytearray(512)
        h[0] = 10
        h[1:11] = b'tmp/a.text'
        h[0x180:0x185] = b'title'
        blk.append(tag(-5, 0, 2) + bytes(h))
        blk.append(tag(5, 0, 3) + b'x' * 512)
        blk.append(tag(5, 1, 4) + b'y' * 512)
        return b''.join(blk)

    def test_files(self):
        with tempfile.TemporaryDirectory() as d:
            img = os.path.join(d, 'vol')
            with open(img, 'wb') as f:
                f.write(self.fixture())
            out = os.path.join(d, 'out')
            self.assertEqual(install(Vol(img), out), 1)
            self.assertEqual(sorted(os.listdir(out)), ['.boot', '.tmp%2Fa.text', 'tmp%2Fa.text'])
            self.assertEqual(read(out, 'tmp%2Fa.text'), b'x' * 512 + b'y' * 188)
            meta = read(out, '.tmp%2Fa.text').decode().split('\n')
            self.assertEqual(meta[0], 'file 5')
            self.assertEqual(struct.unpack('>I', bytes.fromhex(meta[1].split()[1])[48:52])[0], 700)
            self.assertEqual(bytes.fromhex(meta[2].split()[1])[0x180:0x185], b'title')
            boot = read(out, '.boot')
            self.assertEqual((len(boot), boot[:4], boot[24:28]), (536, b'\0\0\0\0', b'BBBB'))

    @unittest.skipUnless(os.path.exists(IMAGE), 'no Lisa ProFile image')
    def test_user_volume(self):
        before = read(IMAGE)
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(install(Vol(IMAGE), d), 134)
            self.assertEqual(os.path.getsize(os.path.join(d, 'system.os')), 141824)
            h = bytes.fromhex(read(d, '.{D26T5}').decode().split('\n')[2].split()[1])
            self.assertIn(b'Getting Started', h[0x180:0x200])
            self.assertEqual(os.path.getsize(os.path.join(d, '.boot')), 30 * 536)
            self.assertEqual(read(d, '.volume').split(b'\n')[0], b'name AOS 3.0')
        self.assertEqual(read(IMAGE), before)


if __name__ == '__main__':
    unittest.main()
