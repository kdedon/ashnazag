# mkapp.py code.bin out -- an AppleSingle Mac application of one CODE
# segment (entry at its start): CODE 0 jump table, CODE 1, SIZE -1.
import struct, sys

code = open(sys.argv[1], 'rb').read()
res = [
    (b'CODE', 0, struct.pack('>IIII', 40, 16, 8, 32) + struct.pack('>HHHH', 0, 0x3f3c, 1, 0xa9f0)),
    (b'CODE', 1, struct.pack('>HH', 0, 1) + code),
    (b'SIZE', -1, struct.pack('>HII', 0x0080, 256 << 10, 256 << 10)),
]
data = b''
offs = []
for t, i, b in res:
    offs.append(len(data))
    data += struct.pack('>I', len(b)) + b
types = []
for t, i, b in res:
    if t not in types:
        types.append(t)
tl = struct.pack('>H', len(types) - 1)
refs = b''
reflen = 2 + 8 * len(types)
for t in types:
    ents = [(i, o) for (tt, i, b), o in zip(res, offs) if tt == t]
    tl += t + struct.pack('>HH', len(ents) - 1, reflen + len(refs))
    for i, o in ents:
        refs += struct.pack('>hhII', i, -1, o, 0)
rmap = bytes(16) + bytes(4 + 2) + struct.pack('>HHH', 0, 28, 28 + len(tl) + len(refs)) + tl + refs
rmap = rmap[:24] + struct.pack('>HH', 28, len(rmap)) + rmap[28:]
fork = struct.pack('>IIII', 256, 256 + len(data), len(data), len(rmap)) + bytes(240) + data + rmap
finf = b'APPLmtcp' + bytes(24)
# laid out as A/UX writes AppleSingle: home "Macintosh", Finder info at 0xe0
hdr = struct.pack('>II', 0x00051600, 0x00010000) + b'Macintosh'.ljust(16) + struct.pack('>H', 2)
o1 = 0xe0
hdr += struct.pack('>III', 9, o1, len(finf)) + struct.pack('>III', 2, o1 + len(finf), len(fork))
open(sys.argv[2], 'wb').write(hdr.ljust(o1, b'\0') + finf + fork)
