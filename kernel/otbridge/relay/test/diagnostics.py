#!/usr/bin/env python3
import struct,sys
from pathlib import Path

def elf(path):
    b=Path(path).read_bytes()
    assert b[:6]==b'\x7fELF\x01\x02'
    sh,sz,count,names=struct.unpack_from('>I',b,32)[0],*struct.unpack_from('>3H',b,46)
    headers=[struct.unpack_from('>10I',b,sh+i*sz) for i in range(count)]
    def section(i):
        h=headers[i];return b[h[4]:h[4]+h[5]]
    strings=section(names);sections={}
    for i,h in enumerate(headers):
        name=strings[h[0]:].split(b'\0',1)[0].decode();sections[name]=section(i)
    symbols={}
    for h in headers:
        if h[1]!=2:continue
        strings=section(h[6]);table=b[h[4]:h[4]+h[5]]
        for at in range(0,len(table),h[9]):
            name,value,size,info,other,index=struct.unpack_from('>IIIBBH',table,at)
            name=strings[name:].split(b'\0',1)[0].decode()
            if index and index<count:symbols[name]=section(index)[value:value+size]
    return sections,symbols

_,normal=elf(sys.argv[1]);_,diagnostic=elf(sys.argv[2])
for name,kind in [('tcpinstall',1),('udpinstall',2)]:
    a,b=normal[name],diagnostic[name]
    assert len(a)==24 and len(b)==88 and a==b[:24],name
    assert struct.unpack_from('>3I',b,24)==(0x4f544447,1,kind)
    assert b[36:]==bytes(52)
text,_=elf(sys.argv[3]);b=text['.text']
assert len(b)==12 and b[:2]==bytes.fromhex('41f9') and b[6:8]==bytes.fromhex('4ef9')
_,cfg=elf(sys.argv[4]);b=cfg['otr_config_diagnostics']
assert len(b)==64 and struct.unpack_from('>3I',b)==(0x4f544447,1,3) and b[12:]==bytes(52)
print('PASS unchanged 24-byte install prefixes, demand-time diagnostics, and 12-byte setup tail wrapper')
