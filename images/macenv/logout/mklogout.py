# mklogout.py app.bin da.bin dir [%System]: the Log Out application as
# dir/Log Out and dir/%Log Out (AppleDouble); with a System file, the
# desk accessory added to it as DRVR 30 "\0Log Out".
import sys, os, struct
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../../tools'))
import macbin2ad, rsrcedit

def rfork(res):
    data, refs, names, types = b'', [], b'', []
    for t, i, nm, b in res:
        refs.append((t, i, len(data), nm))
        data += struct.pack('>I', len(b)) + b
        if t not in types:
            types.append(t)
    tl = struct.pack('>H', len(types) - 1)
    rl = b''
    for t in types:
        ents = [r for r in refs if r[0] == t]
        tl += t + struct.pack('>HH', len(ents) - 1, 2 + 8 * len(types) + len(rl))
        for _, i, o, nm in ents:
            rl += struct.pack('>hhII', i, -1, o, 0)
    m = bytes(24) + struct.pack('>HH', 28, 28 + len(tl) + len(rl)) + tl + rl
    return struct.pack('>IIII', 256, 256 + len(data), len(data), len(m)) + bytes(240) + data + m

app, da, out = [open(f, 'rb').read() for f in sys.argv[1:3]] + [sys.argv[3]]
f = rfork([(b'CODE', 0, None, struct.pack('>IIII', 40, 256, 8, 32) + struct.pack('>HHHH', 0, 0x3f3c, 1, 0xa9f0)),
           (b'CODE', 1, None, struct.pack('>HH', 0, 1) + app),
           (b'SIZE', -1, None, struct.pack('>HII', 0x00c0, 64 << 10, 64 << 10))])
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'Log Out'), 'wb').close()
open(os.path.join(out, '%Log Out'), 'wb').write(macbin2ad.ad_header(b'APPLaLGO', len(f)) + f)
if len(sys.argv) > 4:
    d = bytearray(open(sys.argv[4], 'rb').read())
    rsrcedit.add(d, b'DRVR', 30, da, b'\0Log Out')
    open(sys.argv[4], 'wb').write(d)
