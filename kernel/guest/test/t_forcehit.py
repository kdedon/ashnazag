#!/usr/bin/env python3
"""Compare the checked hit-test forwarder with its native stack behavior."""
from pathlib import Path
import re, struct
root=Path(__file__).resolve().parents[3]
s=(root/'kernel/guest/mod/uinter/uipatch.c').read_text()
def array(name):
    x=s.split('static unsigned char '+name+'[] = {',1)[1].split('};',1)[0]
    return bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',x))
code=array('pollentry'); native=array('hitnative')
def run(point,forward):
    mem=bytearray(65536);base=0x1000
    data=code if forward else native;mem[base:base+len(data)]=data
    regs=[0x12340000+i for i in range(15)];regs[11]=0x2000
    struct.pack_into('>hhhh',mem,0x2000,154,411,174,463)
    struct.pack_into('>hh',mem,0x830,*point)
    sp=0xd000;pc=base+(0x72 if forward else 0);ccr=0x10;trace=[]
    def w(a,v,n=4):mem[a:a+n]=(v&((1<<(n*8))-1)).to_bytes(n,'big')
    def r(a,n=4):return int.from_bytes(mem[a:a+n],'big')
    def push(v,n=4):
        nonlocal sp
        sp-=n;w(sp,v,n)
    def pop(n=4):
        nonlocal sp
        v=r(sp,n);sp+=n;return v
    push(0xf000)
    for step in range(1000):
        op=r(pc,2);pc+=2
        if op==0x554f:sp-=2
        elif op==0x2f38:push(r(r(pc,2)));pc+=2
        elif op==0x4857:push(sp)
        elif op==0x486b:raise AssertionError('unexpected displacement')
        elif op in (0x4853,0x2f0b):push(regs[11])
        elif op==0xa871:
            p=pop();v,h=struct.unpack_from('>hh',mem,p)
            struct.pack_into('>hh',mem,p,v-48,h-144)
            ccr=0x10 if (h&65535)+0xff70>65535 else 0
        elif op==0xa8ad:
            rectangle=pop();v,h=struct.unpack('>hh',pop().to_bytes(4,'big'))
            top,left,bottom,right=struct.unpack_from('>hhhh',mem,rectangle)
            w(sp,int(top<=v<bottom and left<=h<right),1)
            regs[8]=rectangle+8;regs[0]=h&0xffff;regs[1]=v&0xffff;ccr &= 0x10
        elif op==0x101f:
            v=r(sp,1);sp+=2;regs[0]=(regs[0]&0xffffff00)|v
            ccr=(ccr&0x10)|(8 if v&128 else 0)|(4 if not v else 0)
        elif op==0x48e7:
            assert r(pc,2)==0xfffe;pc+=2
            for v in reversed(regs):push(v)
        elif op==0x4cdf:
            assert r(pc,2)==0x7fff;pc+=2;regs[:]=[pop() for _ in range(15)]
        elif op==0x42e7:push(ccr,2)
        elif op==0x44df:ccr=pop(2)
        elif op==0x6100:
            off=struct.unpack_from('>h',mem,pc)[0];push(pc+2);pc+=off
        elif op==0x4878:push(r(pc,2));pc+=2
        elif op==0x2f3c:push(r(pc));pc+=4
        elif op==0x42a7:push(0)
        elif op==0x7026:regs[0]=38
        elif op==0x4e40:
            assert r(sp)==0 and r(sp+4)==0x6e
            mode=r(sp+8)
            if mode==2:trace.append((mode,r(sp+78),regs[11]))
            elif mode==3:trace.append((mode,r(sp+18),r(sp+16,2)))
            else:raise AssertionError(mode)
            regs[:]=[0xabcdef00+i for i in range(15)];ccr=0x1f
        elif op==0x4e71:pass
        elif op==0x4fef:sp+=struct.unpack_from('>h',mem,pc)[0];pc+=2
        elif op==0x4e75:
            pc=pop()
            if pc==0xf000:
                assert sp==0xd000
                return regs,ccr,trace
        else:raise AssertionError(hex(op))
    raise AssertionError('unbounded forwarding')
for point in [(214,579),(212,583),(202,111),(202,555),(201,607),(222,600)]:
    original=run(point,False);forward=run(point,True)
    assert forward[:2]==original[:2]
    assert forward[2][0]==(2,((point[0]-48)&65535)<<16|((point[1]-144)&65535),0x2000)
    assert forward[2][1]==(3,original[0][0],original[1])
print('forcehit: 6 native/forwarded Point, Boolean, registers, CCR, stack cases PASS')
