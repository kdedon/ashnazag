#!/usr/bin/env python3
"""Execute both modal polling stubs with changing button state."""
from pathlib import Path
import re, struct, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
src=(root/'kernel/guest/mod/uinter/uipatch.c').read_text()
array=src.split('static unsigned char pollentry[] = {',1)[1].split('};',1)[0]
code=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',array))
with tempfile.TemporaryDirectory() as d:
    elf,binary=Path(d)/'p.elf',Path(d)/'p.bin'
    tc=str(root/'toolchain/linux/bin/m68k-linux-gnu')
    subprocess.run([tc+'-gcc','-m68020','-nostdlib','-Wl,-e,polldown','-Wl,-Ttext=0','-Wl,--build-id=none','-o',str(elf),str(root/'kernel/guest/test/forcequit/poll.s')],check=True)
    subprocess.run([tc+'-objcopy','-O','binary','-j','.text',str(elf),str(binary)],check=True)
    assert binary.read_bytes()==code

def run(up, states, diagnostic=True):
    mem=bytearray(65536);mem[0x1000:0x1000+len(code)]=code
    mem[0x10d7]=int(diagnostic)
    nativecalls=0
    regs=[0x12340000+i for i in range(15)];saved=regs[:]
    pc=0x1000+(0x16 if up else 0);sp=0xc000;z=False;calls=0
    def r16(a):return struct.unpack_from('>H',mem,a)[0]
    def r32(a):return struct.unpack_from('>I',mem,a)[0]
    def put(a,v):struct.pack_into('>I',mem,a,v&0xffffffff)
    def push(v):
        nonlocal sp
        sp-=4;put(sp,v)
    def pop():
        nonlocal sp
        v=r32(sp);sp+=4;return v
    push(0xf000);put(0x1030,0xe000)
    for step in range(500):
        op=r16(pc);pc+=2
        if op==0x48e7:
            assert r16(pc)==0xfffe;pc+=2
            for v in reversed(regs):push(v)
        elif op==0x4cdf:
            assert r16(pc)==0x7fff;pc+=2;regs[:]=[pop() for _ in range(15)]
        elif op==0x6100:
            off=struct.unpack_from('>h',mem,pc)[0];push(pc+2);pc+=off
        elif op==0x42a7:push(0)
        elif op==0x2f3c:push(r32(pc));pc+=4
        elif op==0x4aba:
            off=struct.unpack_from('>h',mem,pc)[0];z=r32(pc+off)==0;pc+=2
        elif op==0x6700:
            off=struct.unpack_from('>h',mem,pc)[0];pc=pc+off if z else pc+2
        elif op==0x7000:regs[0]=0
        elif op==0x1038:regs[0]=(regs[0]&0xffffff00)|mem[r16(pc)];pc+=2
        elif op==0x2f00:push(regs[0])
        elif op==0x4878:push(r16(pc));pc+=2
        elif op==0x7026:regs[0]=38
        elif op==0x4e40:
            assert regs[0]==38 and r32(sp)==0 and r32(sp+4)==0x6e
            if r32(sp+8)==0:
                mem[0x172]=states[min(calls,len(states)-1)];calls+=1
            else:
                assert r32(sp+8)==4 and r32(sp+12)==mem[0x172] and r32(sp+80)==0xf000
                nativecalls+=1
            for i in (0,1,8,9):regs[i]=0xdeadbeef
        elif op==0x4e71:pass
        elif op==0x4fef:
            sp+=struct.unpack_from('>h',mem,pc)[0];pc+=2
        elif op==0x4a38:assert r16(pc)==0x172;pc+=2;z=mem[0x172]==0
        elif op in (0x6600,):
            off=struct.unpack_from('>h',mem,pc)[0];pc=pc+off if not z else pc+2
        elif op==0x4ef9:
            assert r32(pc)==0xe000 and regs==saved and sp==0xc000 and z
            assert nativecalls==(calls if diagnostic else 0)
            return calls,'tracking'
        elif op==0x4e75:
            pc=pop()
            if pc==0xf000:
                assert regs==saved and sp==0xc000
                assert z!=up
                assert nativecalls==(calls if diagnostic else 0)
                return calls,'return'
        else:raise AssertionError(hex(op))
    raise AssertionError('unbounded poll')
assert run(False,[0x80,0x80,0])==(3,'return')
assert run(False,[0])==(1,'return')
assert run(True,[0])==(1,'tracking')
assert run(True,[0x80])==(1,'return')
assert run(False,[0x80,0x80,0],False)==(3,'return')
assert run(True,[0],False)==(1,'tracking')
print('forcepoll: assembled stubs, 6 execution cases pass')
