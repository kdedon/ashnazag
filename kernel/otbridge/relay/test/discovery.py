#!/usr/bin/env python3
"""Check dependency-free diagnostic packaging."""
from pathlib import Path
import struct
import subprocess
import sys
w = Path(sys.argv[1])
nm = Path(__file__).resolve().parents[4] / 'toolchain/bin/m68k-elf-nm'
assert not subprocess.check_output([str(nm), '-u', str(w / 'discovery.o.linked')]).strip()
b = (w / 'AUX Host Internet Library').read_bytes()
data, length = struct.unpack_from('>II', b, 83)
at = 128 + ((data + 127) // 128) * 128
b = b[at:at + length]
mapping = struct.unpack_from('>I', b, 4)[0]
types = mapping + struct.unpack_from('>H', b, mapping + 24)[0]
kinds = [struct.unpack_from('>4sHH', b, types + 2 + i * 8)
         for i in range(struct.unpack_from('>H', b, types)[0] + 1)]
assert dict((kind, n + 1) for kind, n, _ in kinds) == {b'libr': 1, b'aX01': 3}
print('PASS no unresolved symbols or import resources; two diagnostic classes')
