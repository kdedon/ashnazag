# userfinder.py %Finder: a user's Special menu (fmnu 1255), with Log Out
# (Shut Down's command, whose dialog logs out) in place of Restart and
# Shut Down.
import sys, os, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../tools'))
import rsrcedit

f = sys.argv[1]
d = bytearray(open(f, 'rb').read())
o, l, a = rsrcedit.resource(d, b'fmnu', 1255)
r = bytes(d[o:o + l])
h = 13 + r[12] + (r[12] + 1 & 1)
p, items = h, []
while p < l:
	n = 9 + r[p + 8] + (r[p + 8] + 1 & 1)
	items.append(r[p:p + n])
	p += n
k = [i[:4] for i in items].index(b'rest')
assert items[k + 1][:4] == b'shut'
items[k:k + 2] = [b'shut\x81\0\0\0\x07Log Out']
n, = struct.unpack('>H', r[2:4])
rsrcedit.replace(d, b'fmnu', 1255, r[:2] + struct.pack('>H', n - 1) + r[4:h] + b''.join(items))
open(f, 'wb').write(d)
