#!/usr/bin/env python3
# szdd.py -- compress a file as Microsoft's COMPRESS -r does (SZDD:
# LZSS in a 4096-byte window that starts as spaces), to test the
# expanding in startwin -install.
#
#   szdd.py in out
import sys


def compress(data):
    win = bytearray(b' ' * 4096)
    pos = 4096 - 16
    out = bytearray()
    i = 0
    while i < len(data):
        flags = 0
        chunk = bytearray()
        for bit in range(8):
            if i >= len(data):
                break
            # the longest match in the window as it stands (no overlap with
            # what this token writes, which keeps the search simple)
            best, boff = 0, 0
            for off in range(4096):
                n = 0
                while n < 18 and i + n < len(data) and ((off + n - pos) & 4095) >= 18 and \
                        win[(off + n) & 4095] == data[i + n]:
                    n += 1
                if n > best:
                    best, boff = n, off
                    if n == 18:
                        break
            if best >= 3:
                chunk += bytes([boff & 255, (boff >> 4 & 0xf0) | (best - 3)])
                n = best
            else:
                flags |= 1 << bit
                chunk.append(data[i])
                n = 1
            for k in range(n):
                win[pos] = data[i]
                pos = (pos + 1) & 4095
                i += 1
        out.append(flags)
        out += chunk
    return bytes(out)


data = open(sys.argv[1], 'rb').read()
name = sys.argv[1].rsplit('/', 1)[-1]
hdr = b'SZDD\x88\xf0\x27\x33A' + name[-1:].upper().encode() + len(data).to_bytes(4, 'little')
open(sys.argv[2], 'wb').write(hdr + compress(data))
