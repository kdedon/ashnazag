# mkinit.py init.bin dir: the SoundOut extension (type INIT, one 'INIT'
# resource, locked) as dir/SoundOut and dir/%SoundOut (AppleDouble).
import sys, os, struct
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../../tools'))
import macbin2ad

code, out = open(sys.argv[1], 'rb').read(), sys.argv[2]
data = struct.pack('>I', len(code)) + code
tl = struct.pack('>H', 0) + b'INIT' + struct.pack('>HH', 0, 10)
rl = struct.pack('>hhI', 128, -1, 0x10 << 24) + bytes(4)
m = bytes(24) + struct.pack('>HH', 28, 28 + len(tl) + len(rl)) + tl + rl
f = struct.pack('>IIII', 256, 256 + len(data), len(data), len(m)) + bytes(240) + data + m
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'SoundOut'), 'wb').close()
open(os.path.join(out, '%SoundOut'), 'wb').write(macbin2ad.ad_header(b'INITaSND', len(f)) + f)
