#!/usr/bin/env python3
"""Check packaged absolute operands across independent code and A5 load bases."""
from pathlib import Path
import importlib.util, re, struct, subprocess, sys
library, linked, helper, imports, objdump, resource_parser = sys.argv[1:]
spec = importlib.util.spec_from_file_location('resource_parser', resource_parser)
parser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser)
blob = Path(library).read_bytes()
assert blob[0] == 0 and 1 <= blob[1] <= 63
data_size, fork_size = struct.unpack_from('>II', blob, 83)
fork_offset = 128 + ((data_size+127)//128)*128
fork = blob[fork_offset:fork_offset+fork_size]
assert len(fork) == fork_size
blob = struct.pack('>II16sHIII', 0x00051607, 0x20000, bytes(16), 1, 2, 38, len(fork)) + fork
at, size, _ = parser.resource(blob, b'aX02', 2)
code = blob[at:at+size]
assert code[:4] == bytes.fromhex('ffff0000')
def u32(b, at): return struct.unpack_from('>I', b, at)[0]
def relocations(at):
    result = set(); position = 0
    while True:
        delta = code[at]; at += 1
        if delta == 0:
            if code[at] == 0: return result
            delta = u32(code, at); at += 4
        elif delta & 0x80:
            delta = ((delta & 0x7f) << 8) | code[at]; at += 1
        position += 2 * delta
        assert 40 <= position <= len(code)-4 and position not in result
        result.add(position)
a5rel, segrel = relocations(u32(code, 20)), relocations(u32(code, 28))
assert not a5rel & segrel
elf = Path(linked).read_bytes()
assert elf[:6] == b'\x7fELF\x01\x02'
shoff = u32(elf, 32)
shsize, count, strings = struct.unpack_from('>HHH', elf, 46)
sections = [struct.unpack_from('>10I', elf, shoff+i*shsize) for i in range(count)]
names = sections[strings]; names = elf[names[4]:names[4]+names[5]]
def secname(s): return names[s[0]:].split(b'\0')[0]
textindex = next(i for i,s in enumerate(sections) if secname(s) == b'.text')
text = sections[textindex]
linked_text = elf[text[4]:text[4]+text[5]]
relocated_bytes = {at+i for at in a5rel | segrel for i in range(4)}
for offset, value in enumerate(linked_text):
    if 40+offset not in relocated_bytes:
        assert code[40+offset] == value, 'packaged instruction differs from the built object'
symsec = next(s for s in sections if s[1] == 2)
strsec = sections[symsec[6]]; strs = elf[strsec[4]:strsec[4]+strsec[5]]
symbols = {}
for at in range(symsec[4], symsec[4]+symsec[5], symsec[9]):
    name, value, size, info, other, section = struct.unpack_from('>IIIBBH', elf, at)
    symbols[strs[name:].split(b'\0')[0].decode()] = (value, size, section)
start, length, section = symbols['otr_admit_configuration']
assert section == textindex and length > 0
stubs = [symbols[n][0] for n in ['OTCfigNumberOfChildren','OTCfigGetProviderNameD0','OTCfigChangeProviderName']]
ranges = [(start,start+length)] + [(s,s+34) for s in stubs]
slots = []
for s in sections:
    if s[1] != 4 or s[7] != textindex: continue
    for at in range(s[4],s[4]+s[5],s[9]):
        offset, info, addend = struct.unpack_from('>IIi',elf,at)
        if any(lo <= offset < hi for lo,hi in ranges):
            assert info & 255 == 1, 'helper/import operand is not absolute R_68K_32'
            slot = 40+offset
            assert slot in a5rel | segrel
            assert code[slot-2:slot] in [bytes.fromhex(x) for x in ['4eb9','4879','41f9','2039']]
            slots.append(slot)
assert len(slots) == 11, slots
for obj in [helper,imports]:
    dis = subprocess.check_output([objdump,'-dr',obj],text=True)
    assert not re.search(r'%a5(?:@|\b)',dis), 'runtime A5 operand in helper/import stubs'
checks = 0
for own_a5, own_code in [(0x310000,0x710000),(0x480000,0x920000)]:
    loaded = bytearray(code)
    for slot in a5rel | segrel:
        base = own_a5 if slot in a5rel else own_code
        struct.pack_into('>I',loaded,slot,(u32(code,slot)+base)&0xffffffff)
    for native_a5 in [0x110000,0x5a0000,0x7f0000]:
        assert native_a5 != own_a5
        for slot in slots:
            base = own_a5 if slot in a5rel else own_code
            assert u32(loaded,slot) == (u32(code,slot)+base)&0xffffffff
            checks += 1
print('PASS',checks,'packaged absolute helper/import operands with separate load bases and native A5; no runtime A5 operands')
