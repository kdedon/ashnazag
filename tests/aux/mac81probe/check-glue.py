#!/usr/bin/env python3
"""Execute linked manager glue against a register-clobbering Pascal dispatcher."""
from pathlib import Path
import subprocess
import sys

build = Path(sys.argv[1])
nm = sys.argv[2]
symbols = {}
for line in subprocess.check_output([nm, str(build / 'probe.elf')], text=True).splitlines():
    words = line.split()
    if len(words) == 3:
        symbols[words[2]] = int(words[0], 16)
code = (build / 'probe.bin').read_bytes()
count = 0
for name, trap, selector in [('cfmcall',0xaa5a,True), ('dragcall',0xabed,False),
                             ('threadcall',0xabf2,False), ('aliascall',0xa823,False),
                             ('aecall',0xa816,False)]:
    for nargs in ([2,8,11,13,14] if name=='aecall' else [0,1,4,6]):
        for result in [0,-43]:
            mem = bytearray(8192)
            regs = [0x10000+i for i in range(16)]
            regs[15] = 4096
            original = regs[:]
            def put(a,v,n): mem[a:a+n] = (v & ((1<<(8*n))-1)).to_bytes(n,'big')
            def get(a,n): return int.from_bytes(mem[a:a+n],'big')
            def push(v,n): regs[15]-=n; put(regs[15],v,n)
            def pop(n):
                v=get(regs[15],n); regs[15]+=n; return v
            put(4096,0xdeadbeef,4)
            width=2 if name=='aecall' else 4
            sel=(nargs<<8)|0x17 if name=='aecall' else 5
            put(4100,1024,4); put(4104,nargs,4); put(4108,sel,4)
            for i in range(nargs): put(1024+i*width,0x8100+i,width)
            pc=symbols[name]; steps=0
            while steps<100:
                steps+=1; op=int.from_bytes(code[pc:pc+2],'big'); pc+=2
                if op in [0x206f,0x222f,0x202f]:
                    off=int.from_bytes(code[pc:pc+2],'big');pc+=2
                    regs[{0x206f:8,0x222f:1,0x202f:0}[op]]=get(regs[15]+off,4)
                elif op==0x48e7:
                    assert code[pc:pc+2]==bytes.fromhex('3f3e');pc+=2
                    for r in reversed([*range(2,8),*range(10,15)]): push(regs[r],4)
                elif op==0x4267: push(0,2)
                elif op==0x4a81: zero=regs[1]==0
                elif op==0x6706:
                    if zero: pc+=6
                elif op in [0x2f18,0x3f18]:
                    width=2 if op==0x3f18 else 4
                    push(get(regs[8],width),width);regs[8]+=width
                elif op==0x5381: regs[1]-=1
                elif op==0x60f6: pc-=10
                elif op==0x3f00: push(regs[0],2)
                elif op==trap:
                    if selector: assert pop(2)==5
                    if name=='aecall': assert regs[0]==sel
                    for i in reversed(range(nargs)): assert pop(width)==0x8100+i
                    put(regs[15],result,2)
                    for r in [*range(2,8),*range(10,15)]: regs[r]=0xbad000+r
                elif op==0x301f: regs[0]=pop(2)
                elif op==0x48c0:
                    if regs[0]&0x8000: regs[0]-=65536
                elif op==0x4cdf:
                    assert code[pc:pc+2]==bytes.fromhex('7cfc');pc+=2
                    for r in [*range(2,8),*range(10,15)]: regs[r]=pop(4)
                elif op==0x4e75:
                    assert pop(4)==0xdeadbeef
                    break
                else: raise AssertionError(hex(op))
            else: raise AssertionError('unbounded glue')
            assert regs[0]==result and regs[15]==4100
            assert all(regs[r]==original[r] for r in [*range(2,8),*range(10,15)])
            count+=1
print('manager glue: %d checks passed' % count)
