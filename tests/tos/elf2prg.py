# elf2prg.py BIN0 BIN1 BSS OUT -- a TOS program from one image linked at
# 0 and at 0x10000: the longs that differ by 0x10000 are relocated.
import struct, sys

a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
bss = int(sys.argv[3], 0)
assert len(a) == len(b) and len(a) % 2 == 0
rel = [o for o in range(0, len(a) - 3, 2)
       if struct.unpack_from('>I', b, o)[0] - struct.unpack_from('>I', a, o)[0] == 0x10000]
r = b''
if rel:
    r = struct.pack('>I', rel[0])
    for p, o in zip(rel, rel[1:]):
        d = o - p
        while d > 254:
            r += b'\x01'
            d -= 254
        r += bytes([d])
    r += b'\x00'
else:
    r = struct.pack('>I', 0)
hdr = struct.pack('>HIIIIIIH', 0x601a, len(a), 0, bss, 0, 0, 0, 0)
open(sys.argv[4], 'wb').write(hdr + a + r)
