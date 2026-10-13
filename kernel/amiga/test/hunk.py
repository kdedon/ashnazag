#!/usr/bin/env python3
# hunk.py -- wrap position-independent 68k code as an AmigaOS executable.
#
#   hunk.py code.bin out
import struct, sys
code = open(sys.argv[1], 'rb').read()
code += b'\0' * (-len(code) % 4)
n = len(code) // 4
open(sys.argv[2], 'wb').write(struct.pack('>7L', 0x3F3, 0, 1, 0, 0, n, 0x3E9) +
	struct.pack('>L', n) + code + struct.pack('>L', 0x3F2))
