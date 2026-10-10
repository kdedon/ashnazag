"""Package a classic application with space for its QuickDraw globals."""
import pathlib
import runpy
import struct
import sys

root = pathlib.Path(__file__).resolve().parents[3]
runpy.run_path(str(root / "tests/net/mtcp/mkapp.py"), run_name="__main__")
sys.path.insert(0, str(root / "tools"))
import rsrcedit

path = pathlib.Path(sys.argv[2])
data = bytearray(path.read_bytes())
off, length, attrs = rsrcedit.resource(data, b"CODE", 0)
assert length == 24 and struct.unpack_from(">IIII", data, off) == (40, 16, 8, 32)
struct.pack_into(">I", data, off + 4, 256)
off, length, attrs = rsrcedit.resource(data, b"SIZE", -1)
assert length == 10
flags = struct.unpack_from(">H", data, off)[0]
struct.pack_into(">H", data, off, flags | 0x0040)
path.write_bytes(data)
