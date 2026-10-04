#!/usr/bin/env python3
"""Extract a validated handler HUNK into extension-ROM build inputs."""
import struct
import sys
from pathlib import Path
from check import check

source, out = Path(sys.argv[1]), Path(sys.argv[2])
check(source)
raw = source.read_bytes()
words = list(struct.unpack('>'+'I'*(len(raw)//4), raw))
size = words[7]*4
at = 8+words[7]
relocs = []
if words[at] == 1004:
    count = words[at+1]
    relocs = words[at+3:at+3+count]
(out/'handler-image.bin').write_bytes(raw[32:32+size])
(out/'handler-image.inc').write_text(f'.equ HANDLER_SIZE, {size}\n.equ HANDLER_RELOCS, {len(relocs)}\n')
(out/'handler-relocs.inc').write_text(''.join(f'.long {r}\n' for r in relocs))
