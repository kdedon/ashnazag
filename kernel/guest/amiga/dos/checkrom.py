#!/usr/bin/env python3
"""Check resident discovery and the linked cold-start bootstrap without execution."""
import struct
import sys
from pathlib import Path

BASE = 0xf00000
rom = Path(sys.argv[1]).read_bytes()
assert len(rom) == 0x80000
word = lambda offset: struct.unpack_from('>H', rom, offset)[0]
long = lambda offset: struct.unpack_from('>I', rom, offset)[0]
assert word(0) == 0x4afc and long(2) == BASE
end = long(6)
assert BASE + 26 < end < BASE + len(rom)
assert rom[10:14] == bytes([1, 1, 0, 217])
for offset in (14, 18, 22):
    assert BASE + 26 <= long(offset) < end
assert rom[long(14)-BASE:].split(b'\0', 1)[0] == b'container.boot'
assert rom[long(22)-BASE:long(22)-BASE+2] == b'\x48\xe7'
tag = end - BASE
assert word(tag) == 0x4afc and long(tag+2) == end and rom[tag+10:tag+14] == bytes([4, 1, 0, 146])
assert rom[long(tag+14)-BASE:].split(b'\0', 1)[0] == b'container.rtg'
assert long(tag+6) > end and rom[long(tag+22)-BASE:long(tag+22)-BASE+2] == b'\x48\xe7'
assert b'\x4e\xae\xff\xa0' in rom  # FindResident
assert b'\x4e\xae\xff\x9a' in rom  # InitResident
assert b'\x4e\xae\xff\xdc' in rom  # AddBootNode
# the cold-start residents binding the board before DOS, around the boot menu
for name, pri in ((b'container.early', -45), (b'container.menu', -51)):
    at = rom.find(name + b'\0')
    tags = [o for o in range(0, at, 2) if word(o) == 0x4afc and long(o+2) == BASE+o and long(o+14) == BASE+at]
    assert len(tags) == 1 and rom[tags[0]+10] == 1 and rom[tags[0]+13] == pri & 255, name
print(f'{sys.argv[1]}: native cold-start resident, {end-BASE} linked bytes, DOS diagnostic boot entry, after-DOS RTG resident')
