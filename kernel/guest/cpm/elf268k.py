# elf268k.py BIN0 BIN1 BSS OUT -- a relocatable CP/M-68K program from one
# image linked at 0 and at 0x10000: the longs that differ by 0x10000 are
# relocated, all against the text base.
import struct, sys

a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
bss = int(sys.argv[3], 0)
assert len(a) == len(b)
if len(a) % 2:
    a += b'\0'
    b += b'\0'
rel = bytearray(len(a))
o = 0
while o + 4 <= len(a):
    if struct.unpack_from('>I', b, o)[0] - struct.unpack_from('>I', a, o)[0] == 0x10000:
        struct.pack_into('>HH', rel, o, 5, 2)
        o += 4
    else:
        o += 2
hdr = struct.pack('>HIIIIIIH', 0x601a, len(a), 0, bss, 0, 0, 0, 0)
open(sys.argv[4], 'wb').write(hdr + a + bytes(rel))
