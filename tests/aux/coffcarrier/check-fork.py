#!/usr/bin/env python3
"""Execute the linked fork bridge through its parent and child exits."""
import pathlib, struct, subprocess, sys
elf=pathlib.Path(sys.argv[1]); root=pathlib.Path(__file__).resolve().parents[3]
nm=root/'toolchain/linux/bin/m68k-linux-gnu-nm'
symbols={line.split()[2]:int(line.split()[0],16) for line in subprocess.check_output([str(nm),str(elf)],text=True).splitlines() if len(line.split())==3}
code=elf.with_suffix('.bin').read_bytes()
def run(d0,d1,ccr,child):
    regs=[0x1000+i for i in range(16)]; regs[15]=sp=0x700000; initial=regs[:]; mem={}; writes=[]
    raw=0x710000; mask=32; pc=symbols['auxspawn']; trapped=False
    def put(a,v,n):
        writes.append((a,n));
        for i in range(n):mem[a+i]=(v>>(8*(n-1-i)))&255
    def get(a,n):return sum(mem.get(a+i,0)<<(8*(n-1-i)) for i in range(n))
    def word(a):return int.from_bytes(code[a:a+2],'big')
    def long(a):return int.from_bytes(code[a:a+4],'big')
    put(sp,0xabcdef,4);put(sp+4,raw,4);put(sp+8,mask,4); writes.clear()
    for step in range(80):
        op=word(pc); pc+=2
        if op==0x48e7:
            assert word(pc)==0x3f3e;pc+=2
            for r in reversed([2,3,4,5,6,7,10,11,12,13,14]):regs[15]-=4;put(regs[15],regs[r],4)
        elif op in (0x266f,0x246f):
            r=11 if op==0x266f else 10;regs[r]=get(regs[15]+word(pc),4);pc+=2
        elif op==0x7002:regs[0]=2
        elif op==0x4e40:
            assert not trapped;trapped=True;regs[0]=d0;regs[1]=d1;flags=ccr
        elif op==0x42c2:regs[2]=(regs[2]&0xffff0000)|flags
        elif op==0x0802:
            assert word(pc)==0;pc+=2;z=not(regs[2]&1)
        elif op==0x4a01:z=not(regs[1]&255)
        elif op>>8 in (0x66,0x67):
            disp=op&255;disp=disp-256 if disp>127 else disp
            if (not z if op>>8==0x66 else z):pc+=disp
        elif op==0x4ff9:regs[15]=long(pc);pc+=4
        elif op in (0x2740,0x2741,0x3742,0x274f):
            r={0x2740:0,0x2741:1,0x3742:2,0x274f:15}[op];n=2 if op==0x3742 else 4
            put(regs[11]+word(pc),regs[r],n);pc+=2
        elif op==0x2680:put(regs[11],regs[0],4)
        elif op==0x2f0a:regs[15]-=4;put(regs[15],regs[10],4)
        elif op==0x4ebb:
            assert word(pc)==0x0170;target=pc+long(pc+2);assert target==symbols['childexec'];assert child
            assert regs[15]==0x3ffffefc and get(regs[15],4)==mask
            assert get(raw+16,4)==d0 and get(raw+20,4)==d1 and get(raw+24,2)==ccr and get(raw+28,4)==0x3fffff00
            assert not any(sp-44<=a<sp for a,n in writes[11:]),writes
            return
        elif op==0x4480:regs[0]=(-regs[0])&0xffffffff
        elif op==0x6000:
            disp=word(pc);pc+=disp
        elif op==0x4cdf:
            assert word(pc)==0x7cfc;pc+=2
            for r in [2,3,4,5,6,7,10,11,12,13,14]:regs[r]=get(regs[15],4);regs[15]+=4
        elif op==0x4e75:
            assert not child and regs[15]==sp and get(sp,4)==0xabcdef
            assert all(regs[r]==initial[r] for r in [2,3,4,5,6,7,10,11,12,13,14])
            assert get(raw,4)==d0 and get(raw+4,4)==d1 and get(raw+8,2)==ccr and get(raw+12,4)==sp-44
            assert regs[0]==((-d0)&0xffffffff if ccr&1 else d0)
            return
        else:raise AssertionError((hex(pc-2),hex(op)))
    raise AssertionError('fork path did not terminate')
for args in [(24,0,0,False),(24,0x100,0,False),(24,1,0,True),(24,0x101,0,True),(11,0,1,False),(11,1,1,False)]:run(*args)
print('carrier: 6 linked fork paths PASS')
for d0,d1,carry in [(50,9,False),(50,0x7f00,False),(10,0,True)]:
    pc=symbols['auxwait'];sp=0x700000;status=0x710000;stored=None
    for step in range(12):
        op=int.from_bytes(code[pc:pc+2],'big');pc+=2
        if op==0x7000:pass
        elif op==0x303c:assert int.from_bytes(code[pc:pc+2],'big')==151;pc+=2
        elif op==0x4e40:pass
        elif op>>8==0x64:
            if not carry:pc+=op&255
        elif op==0x4480:d0=(-d0)&0xffffffff
        elif op==0x206f:assert int.from_bytes(code[pc:pc+2],'big')==8;pc+=2
        elif op==0x2081:stored=d1
        elif op==0x4e75:
            assert stored==(None if carry else d1)
            assert d0==((-10)&0xffffffff if carry else 50)
            break
        else:raise AssertionError((hex(pc-2),hex(op)))
    else:raise AssertionError('wait bridge did not return')
print('carrier: 3 linked wait-result paths PASS')
