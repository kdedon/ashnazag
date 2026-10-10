#!/usr/bin/env python3
"""Execute the assembled pointer-relocation instructions with distinct bases."""
from pathlib import Path
import struct,sys
b=Path(sys.argv[1]).read_bytes()
shoff=struct.unpack_from('>I',b,32)[0]
size,count,strings=struct.unpack_from('>HHH',b,46)
sections=[struct.unpack_from('>10I',b,shoff+i*size) for i in range(count)]
st=sections[strings];names=b[st[4]:st[4]+st[5]]
section=next(s for s in sections if names[s[0]:].split(b'\0')[0]==b'.a5init')
code=b[section[4]:section[4]+section[5]]
start=code.index(bytes.fromhex('201941f308002410'))
assert code.count(bytes.fromhex('201941f308002410'))==1

def execute(value,jump_base,opcode=0x4ef9):
    a5,base,slot,offset=0x300000,0x2fff00,0x2fff40,0x40
    target=0x750000+value if value>0 else 0x750000
    memory={slot:value&0xffffffff}
    d0=d2=0;a0=a4=0;pc=start;z=n=False;reads=0
    for step in range(32):
        op=int.from_bytes(code[pc:pc+2],'big');pc+=2
        if op==0x2019:d0=offset
        elif op==0x41f3:
            assert code[pc:pc+2]==b'\x08\x00';pc+=2;a0=base+d0
        elif op==0x2410:d2=memory[a0];z=d2==0;n=bool(d2&0x80000000)
        elif op in (0x6f18,0x6712,0x661a,0x6004):
            cc=op>>8;delta=op&255
            if cc==0x60 or (cc==0x6f and (z or n)) or (cc==0x67 and z) or (cc==0x66 and not z):pc+=delta
        elif op==0x4aae:
            assert code[pc:pc+2]==b'\x00\x10';pc+=2;z=jump_base==0;n=False
        elif op==0x286e:
            assert code[pc:pc+2]==b'\x00\x10';pc+=2;a4=jump_base
        elif op==0xd9c2:a4=(a4+d2)&0xffffffff
        elif op==0x0c54:
            assert code[pc:pc+2]==b'\x4e\xf9';pc+=2
            assert a4==jump_base+value;reads+=1;z=opcode==0x4ef9;n=False
        elif op==0x20ac:
            assert code[pc:pc+2]==b'\x00\x02';pc+=2;memory[a0]=target
        elif op==0xd483:d2=(d2+a5)&0xffffffff
        elif op==0x2082:memory[a0]=d2
        elif op==0x5381:return 0,memory[slot],reads
        elif op==0x70ff:return -1,memory[slot],reads
        else:raise AssertionError((hex(op),hex(pc-2)))
    raise AssertionError('instruction bound exceeded')

checks=0
for value in [-256,-128,-4,0,0x22,0x42,0x82,0x122]:
    for base in [0,0x880000,0x990000]:
        status,actual,reads=execute(value,base)
        expected=(0x750000+value) if value>0 and base else (0x300000+value)&0xffffffff
        assert status==0 and actual==expected
        assert reads==int(value>0 and base!=0)
        checks+=1
for value in [0x22,0x42,0x82,0x122]:
    for opcode in [0,0x3f3c,0x4e75]:
        status,actual,reads=execute(value,0x880000,opcode)
        assert status==-1 and actual==value and reads==1
        checks+=1
print('PASS',checks,'assembled data/code relocation, distinct jump bases, zero fallback and invalid JMP cases')
