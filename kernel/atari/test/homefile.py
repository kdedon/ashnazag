#!/usr/bin/env python3
"""homefile.py -- whether a Falcon disk image's /home holds a path.

    homefile.py IMAGE guest/TOS/CUBASE/CUBASE31

Exit 0 if it does, 1 if not.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'tools'))
from ufs import UFS


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        rs = f.read(512)
    for i in range(4):
        flg, pid, st, n = struct.unpack_from('>B3sII', rs, 0x1C6 + 12 * i)
        if flg & 1 and pid == b'AXU':
            break
    else:
        sys.exit(1)
    u, ino = UFS(sys.argv[1], st * 512), 2
    for name in sys.argv[2].strip('/').split('/'):
        ino = dict(u.readdir(ino)).get(name)
        if ino is None:
            sys.exit(1)
    sys.exit(0)


if __name__ == '__main__':
    main()
