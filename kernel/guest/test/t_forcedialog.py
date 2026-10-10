#!/usr/bin/env python3
"""Run the production foreground guard with mocked public Toolbox calls."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
src = root / 'kernel/guest/mod/uinter/uipatch.c'
array = src.read_text().split('static unsigned char forcecode[] = {',1)[1].split('};',1)[0]
code = bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',array))
with tempfile.TemporaryDirectory(prefix='forcedialog-') as tmp:
    elf, binary = Path(tmp)/'stub.elf', Path(tmp)/'stub.bin'
    tc = root/'toolchain/linux/bin/m68k-linux-gnu'
    subprocess.run([str(tc)+'-gcc','-m68020','-nostdlib','-Wl,-e,entry','-Wl,-Ttext=0','-Wl,--build-id=none','-o',str(elf),str(root/'kernel/guest/test/forcequit/foreground.s')],check=True)
    subprocess.run([str(tc)+'-objcopy','-O','binary','-j','.text',str(elf),str(binary)],check=True)
    assert code == binary.read_bytes()

def run(front,current,front_error=0,current_error=0):
    mem=bytearray(4096); pc=0; sp=2048; fp=0x12345678; d0=0; z=False; calls=[]
    def r16(a): return struct.unpack_from('>H',mem,a)[0]
    def r32(a): return struct.unpack_from('>I',mem,a)[0]
    def w16(a,v): struct.pack_into('>H',mem,a,v&0xffff)
    def w32(a,v): struct.pack_into('>I',mem,a,v&0xffffffff)
    def push(n,v):
        nonlocal sp
        sp-=n; (w16 if n==2 else w32)(sp,v)
    push(4,0xffffffff)
    for steps in range(100):
        op=struct.unpack_from('>H',code,pc)[0]; pc+=2
        def word():
            nonlocal pc
            v=struct.unpack_from('>h',code,pc)[0]; pc+=2; return v
        if op==0x4e56:
            offset=word(); push(4,fp); fp=sp; sp+=offset
        elif op==0x4267: push(2,0); z=True
        elif op==0x486e: push(4,fp+word())
        elif op&0xff00==0x7000: d0=op&255; d0=d0-256 if d0&128 else d0; z=d0==0
        elif op==0x2f00: push(4,d0)
        elif op==0x3f3c: push(2,word())
        elif op==0xa88f:
            selector=r16(sp)
            if selector==0x39:
                assert r32(sp+2)==0xffffffff
                ptr=r32(sp+6); psn,error,size=front,front_error,10
            else:
                assert selector==0x37
                ptr=r32(sp+2); psn,error,size=current,current_error,6
            w32(ptr,psn[0]); w32(ptr+4,psn[1]); w16(sp+size,error); sp+=size
        elif op==0x301f: d0=(d0&0xffff0000)|r16(sp); sp+=2; z=(d0&0xffff)==0
        elif op in (0x6600,0x6000):
            base=pc; offset=word()
            if op==0x6000 or not z: pc=base+offset
        elif op==0x202e: d0=r32(fp+word()); z=d0==0
        elif op==0xb0ae: z=d0==r32(fp+word())
        elif op==0x303c: d0=(d0&0xffff0000)|(word()&0xffff); z=(d0&0xffff)==0
        elif op==0xa9c9:
            assert d0&0xffff==20002; calls.append('SysError20002')
        elif op==0x4e5e: sp=fp; fp=r32(sp); sp+=4
        elif op==0x4e75:
            assert r32(sp)==0xffffffff; sp+=4
            assert sp==2048 and fp==0x12345678
            return d0,calls
        else: raise AssertionError(hex(op))
    raise AssertionError('unbounded stub')

for args, expected in [(((0,8192),(0,8192)),1),(((0,8192),(0,8193)),0),(((1,8192),(0,8192)),0),(((0,8192),(0,8192),-50),0),(((0,8192),(0,8192),0,-50),0),(((0xffffffff,0xffffffff),(0xffffffff,0xffffffff)),1)]:
    result,calls=run(*args)
    assert result==expected and len(calls)==expected
print('forcedialog: 7 checks passed')
