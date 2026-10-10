#!/usr/bin/env python3
"""Execute the native recovery wrapper against mocked public Toolbox calls."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root/'kernel/guest/mod/uinter/uipatch.c').read_text()
array = source.split('static unsigned char wneentry[] = {',1)[1].split('};',1)[0]
code = bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',array))
with tempfile.TemporaryDirectory(prefix='forceentry-') as tmp:
    tc = root/'toolchain/linux/bin/m68k-linux-gnu'
    elf,binary=Path(tmp)/'entry.elf',Path(tmp)/'entry.bin'
    subprocess.run([str(tc)+'-gcc','-m68020','-nostdlib','-Wl,-e,entry','-Wl,-Ttext=0','-Wl,--build-id=none','-o',str(elf),str(root/'kernel/guest/test/forcequit/entry.s')],check=True)
    subprocess.run([str(tc)+'-objcopy','-O','binary','-j','.text',str(elf),str(binary)],check=True)
    assert binary.read_bytes()[:len(code)]==code
    full=binary.read_bytes()
    ext=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',source.split('static unsigned char frontentry[] = {',1)[1].split('};',1)[0]))
    assert full[0x1fe:0x1fe+len(ext)]==ext
    assert not any(full[0x1fe+len(ext):])
assert int(re.search(r'#define FRONTTRAP (0x[0-9a-f]+)',source).group(1),16)==ext.index(b'\x4e\x40')+2
word=0x110

def run(state,ticks,deadline=0,front=(0,7),current=(0,7),errors=(0,0),nested=False,restore_error=False,observe=False,window=0x3000,title=b"Appearance"):
    mem=bytearray(65536)
    mem[0x1000:0x1000+len(full)]=full
    def r16(a): return struct.unpack_from('>H',mem,a)[0]
    def r32(a): return struct.unpack_from('>I',mem,a)[0]
    def w16(a,v): struct.pack_into('>H',mem,a,v&65535)
    def w32(a,v): struct.pack_into('>I',mem,a,v&0xffffffff)
    w32(0x1086,0xf000); w32(0x904,0x8000)
    w32(0x1000+0x1fe+0x9c,1 if observe else 0)
    w32(0x1000+word,state); w32(0x1000+word+4,deadline)
    calls=[]
    def execute(check_nested=False,stackbase=0xc000):
        regs=[0x33440000+i for i in range(15)]
        saved=regs[:]; pc=0x1000; sp=stackbase; z=n=c=False
        params=bytes(range(20)); mem[sp:sp+20]=params
        def push(size,v):
            nonlocal sp
            sp-=size; (w16 if size==2 else w32)(sp,v)
        def pop(size):
            nonlocal sp
            v=(r16 if size==2 else r32)(sp); sp+=size; return v
        def flags(v):
            nonlocal z,n,c
            c=False; v&=0xffffffff; z=v==0; n=bool(v&0x80000000)
        def short():
            nonlocal pc
            v=struct.unpack_from('>h',mem,pc)[0];pc+=2;return v
        def long():
            nonlocal pc
            v=r32(pc);pc+=4;return v
        for steps in range(300):
            op=r16(pc);pc+=2
            if op==0x48e7:
                assert short()&65535==0xfffe
                for v in reversed(regs): push(4,v)
            elif op==0x4cdf:
                assert short()==0x7fff
                regs[:]=[pop(4) for _ in range(15)]
            elif op==0x41fb:
                base=pc; assert short()==0x170
                off=long();off=off-0x100000000 if off&0x80000000 else off
                regs[8]=(base+off)&0xffffffff
            elif op==0x42e7: push(2,(4 if z else 0)|(8 if n else 0)|(1 if c else 0))
            elif op==0x44df:
                ccr=pop(2);z=bool(ccr&4);n=bool(ccr&8);c=bool(ccr&1)
            elif op==0x4abb:
                base=pc;assert short()==0x170;off=long();off=off-0x100000000 if off&0x80000000 else off;flags(r32(base+off))
            elif op==0x2d4d:w32(regs[14]+short(),regs[13])
            elif op==0x2d5f:w32(regs[14]+short(),pop(4))
            elif op==0x4aae:flags(r32(regs[14]+short()))
            elif op==0x2f2e:push(4,r32(regs[14]+short()))
            elif op==0x4878:push(4,short()&65535)
            elif op==0xa924:
                w32(sp,window)
                for i in (0,1,2,8,9):regs[i]=0xdead0000+i
            elif op==0xa919:
                out=r32(sp);assert r32(sp+4)==window
                mem[out]=len(title);mem[out+1:out+1+len(title)]=title;sp+=8
                for i in (0,1,2,8,9):regs[i]=0xdead0000+i
            elif op==0x4a90: flags(r32(regs[8]))
            elif op==0x0c90: flags(r32(regs[8])-long())
            elif op in (0x6700,0x6600,0x6c00,0x6a00,0x6000,0x6100,0x6400):
                base=pc;off=short()
                take={0x6700:z,0x6600:not z,0x6c00:not n,0x6a00:not n,0x6000:True,0x6100:True,0x6400:not c}[op]
                if take:
                    if op==0x6100:push(4,pc)
                    pc=base+off
            elif op==0x2a78: regs[13]=r32(short()&65535)
            elif op==0x42a7: push(4,0);flags(0)
            elif op==0xa975:
                assert regs[13]==0x8000
                calls.append('TickCount');w32(sp,ticks);regs[0]=0xbad
            elif op==0x201f:regs[0]=pop(4);flags(regs[0])
            elif op==0x2200:regs[1]=regs[0];flags(regs[1])
            elif op==0x0681:regs[1]=(regs[1]+long())&0xffffffff;flags(regs[1])
            elif op==0x2141:w32(regs[8]+short(),regs[1]);flags(regs[1])
            elif op==0x20bc:v=long();w32(regs[8],v);flags(v)
            elif op==0x90a8:regs[0]=(regs[0]-r32(regs[8]+short()))&0xffffffff;flags(regs[0])
            elif op==0x4a80:flags(regs[0])
            elif op==0x4290:w32(regs[8],0);flags(0)
            elif op==0x4ef9:
                assert long()==0xf000
                assert regs==saved and sp==stackbase and mem[sp:sp+20]==params
                return
            elif op==0x4e56:
                off=short();push(4,regs[14]);regs[14]=sp;sp+=off
            elif op==0x4267:push(2,0);flags(0)
            elif op==0x486e:push(4,regs[14]+short())
            elif op&0xff00==0x7000:
                v=op&255;regs[0]=(v-256 if v&128 else v)&0xffffffff;flags(regs[0])
            elif op==0x2f00:push(4,regs[0])
            elif op==0x3f3c:push(2,short())
            elif op==0xa88f:
                sel=r16(sp)
                if sel==57:
                    assert r32(sp+2)==0xffffffff
                    ptr=r32(sp+6);psn,error,size=front,errors[0],10
                else:
                    assert sel==55
                    ptr=r32(sp+2);psn,error,size=current,errors[1],6
                w32(ptr,psn[0]);w32(ptr+4,psn[1]);w16(sp+size,error);sp+=size
                for i in (0,1,2,8,9):regs[i]=0xdead0000+i
            elif op==0x301f:regs[0]=(regs[0]&0xffff0000)|pop(2);flags(regs[0]&65535)
            elif op==0x202e:regs[0]=r32(regs[14]+short());flags(regs[0])
            elif op==0xb0ae:flags(regs[0]-r32(regs[14]+short()))
            elif op==0x303c:regs[0]=(regs[0]&0xffff0000)|(short()&65535);flags(regs[0]&65535)
            elif op==0xa9c9:
                assert regs[0]&65535==20002 and regs[13]==0x8000
                assert r32(0x1000+word)==3
                calls.append('SysError20002')
                if nested:execute(True,sp-32)
            elif op==0x2f3c:push(4,long())
            elif op==0x4e40:
                assert regs[0]==38 and r32(sp+4)==0x6e
                if r32(sp+8)==5:
                    rec=r32(sp+12);assert rec==sp+16
                    assert (r32(rec),r32(rec+4))==current and (r32(rec+8),r32(rec+12))==front
                    assert r32(rec+16)==window and r32(rec+20)==0x8000
                    assert mem[rec+24]==len(title) and mem[rec+25:rec+25+len(title)]==title
                    calls.append('FrontSnapshot')
                else:assert r32(sp+8)==1
                for i in (0,1,8,9):regs[i]=0xdead0000+i
                c=restore_error
            elif op==0x4e71:pass
            elif op==0x4fef:sp+=short()
            elif op==0x4e5e:sp=regs[14];regs[14]=pop(4)
            elif op==0x4e75:pc=pop(4)
            else:raise AssertionError(hex(op))
        raise AssertionError('wrapper loop')
    execute()
    return r32(0x1000+word),r32(0x1000+word+4),calls

assert run(0,1000)==(0,0,[])
assert run(3,1000)==(3,0,[])
assert run(1,1000,nested=True)==(0,1300,['TickCount','SysError20002'])
for front in ((0,8),(1,7)):
    assert run(1,1000,front=front)==(2,1300,['TickCount'])
for errors in ((-1,0),(0,-1)):
    assert run(1,1000,errors=errors)==(2,1300,['TickCount'])
assert run(2,1299,1300)==(0,1300,['TickCount','SysError20002'])
assert run(2,1300,1300)==(0,1300,['TickCount'])
assert run(1,0xffffff00,front=(0,8))==(2,44,['TickCount'])
assert run(2,43,44,front=(0,8))==(2,44,['TickCount'])
assert run(2,44,44)==(0,44,['TickCount'])
assert run(1,1000,restore_error=True)==(4,1300,['TickCount','SysError20002'])
assert run(4,1000)==(4,0,[])
assert run(0,1000,observe=True)==(0,0,['FrontSnapshot'])
assert run(0,1000,observe=True,front=(0,8))==(0,0,[])
assert run(0,1000,observe=True,errors=(-1,0))==(0,0,[])
assert run(0,1000,observe=True,errors=(0,-1))==(0,0,[])
assert run(0,1000,observe=True,window=0)==(0,0,[])
assert run(0,1000,observe=True,title=b'x'*255)==(0,0,['FrontSnapshot'])
print('forceentry: 14 recovery +6 public identity checks passed')
