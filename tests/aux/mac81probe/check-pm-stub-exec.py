#!/usr/bin/env python3
"""Compare the owned prefix forwarder with the native prefix instruction boundary."""
import argparse,pathlib,re,struct,subprocess,tempfile
from unicorn import Uc,UC_ARCH_M68K,UC_MODE_BIG_ENDIAN,UC_HOOK_INTR
from unicorn.m68k_const import *
p=argparse.ArgumentParser();p.add_argument('source',type=pathlib.Path);p.add_argument('--toolchain',type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[3]/'toolchain'/'bin');a=p.parse_args();s=a.source.read_text()
def array(name,n):
 m=re.search(name+r'\['+str(n)+r'\]=\{([^}]+)\}',s);assert m
 b=bytearray(int(x,0) for x in re.findall(r'0x[0-9a-f]+|\b\d+\b',m[1]));assert len(b)==n;return b
code=array('pmcode',60);prefix=array('pmprefix',16);native=array('pmnative',8)
site=0x30000;block=0x10000;stack=0x90000;sys=0x80000;record=0x70000
struct.pack_into('>I',code,50,site+8);struct.pack_into('>I',code,56,site)
asm=''' .text
 movem.l %d0-%d7/%a0-%a6,-(%sp)
 move.w %ccr,-(%sp)
 pea 6.w
 move.l #0x6e,-(%sp)
 clr.l -(%sp)
 moveq #38,%d0
 trap #0
 nop
 lea 12(%sp),%sp
 move.w (%sp)+,%ccr
 movem.l (%sp)+,%d0-%d7/%a0-%a6
 cmpi.w #1,%d7
 bne.s idle
 movea.l -0x456(%a5),%a4
 move.w #4,(%a4)
 jmp 0x30008.l
idle:
 jmp 0x30000.l
'''
with tempfile.TemporaryDirectory() as tmp:
 d=pathlib.Path(tmp);(d/'stub.s').write_text(asm)
 subprocess.run([str(a.toolchain/'m68k-elf-as'),'-m68040','-o',str(d/'stub.o'),str(d/'stub.s')],check=True)
 subprocess.run([str(a.toolchain/'m68k-elf-objcopy'),'-O','binary','-j','.text',str(d/'stub.o'),str(d/'stub.bin')],check=True)
 assert (d/'stub.bin').read_bytes()==code
regs=[globals()['UC_M68K_REG_D'+str(i)] for i in range(8)]+[globals()['UC_M68K_REG_A'+str(i)] for i in range(7)]
def machine(ccr,d7,a4):
 u=Uc(UC_ARCH_M68K,UC_MODE_BIG_ENDIAN);u.ctl_set_cpu_model(UC_CPU_M68K_M68040);u.mem_map(0x1000,0x100000)
 u.mem_write(site-16,bytes(prefix+native));u.mem_write(block,bytes(code));u.mem_write(sys-0x456,record.to_bytes(4,'big'))
 initial=[0x10203040+i*0x10101 for i in range(15)];initial[7]=d7;initial[12]=a4;initial[13]=sys
 for r,v in zip(regs,initial):u.reg_write(r,v)
 u.reg_write(UC_M68K_REG_SR,ccr);u.reg_write(UC_M68K_REG_A7,stack);u.mem_write(stack,b'\xa5'*128)
 return u
def ccrvalue(u,at):
 old=bytes(u.mem_read(at,2));sp=u.reg_read(UC_M68K_REG_A7)
 u.mem_write(at,b"\x42\xe7");u.emu_start(at,at+2,count=1)
 value=int.from_bytes(u.mem_read(sp-2,2),"big")&31
 u.mem_write(at,old);u.reg_write(UC_M68K_REG_A7,sp);return value
cases=0
for ccr in range(32):
 for d7 in (0,1,0x10001,2):
  for a4 in (0,0x12345678):
   for error in (0,4):
    golden=machine(ccr,d7,a4);skip=(d7&0xffff)==1
    golden.emu_start(site-16,site+8 if skip else site,count=16)
    expected=[golden.reg_read(r) for r in regs];expectedccr=ccrvalue(golden,site+8 if skip else site)
    u=machine(ccr,d7,a4);u.mem_write(site-16,b'\x4e\xf9'+block.to_bytes(4,'big')+b'\x4e\x71'*5);boundaries=[]
    def interrupt(cpu,n,data):
     pc=cpu.reg_read(UC_M68K_REG_PC);sp=cpu.reg_read(UC_M68K_REG_A7)
     if n==32:
      assert pc==block+20 and sp==stack-74
      raw=bytes(cpu.mem_read(sp,74));assert struct.unpack('>III',raw[:12])==(0,0x6e,6)
      assert struct.unpack('>H',raw[12:14])[0]&31==ccr
      assert list(struct.unpack('>15I',raw[14:]))==[0x10203040+i*0x10101 if i not in (7,12,13) else d7 if i==7 else a4 if i==12 else sys for i in range(15)]
      for r in regs[:2]+regs[8:10]:cpu.reg_write(r,0xdeadbeef)
      cpu.reg_write(UC_M68K_REG_D0,error);cpu.reg_write(UC_M68K_REG_SR,error!=0)
      cpu.reg_write(UC_M68K_REG_PC,pc+2);boundaries.append(n)
     elif n==10:
      assert not skip and pc==site and sp==stack
      assert [cpu.reg_read(r) for r in regs]==expected
      assert bytes(cpu.mem_read(stack,128))==b'\xa5'*128
      cpu.reg_write(UC_M68K_REG_D1,0x31415926);cpu.reg_write(UC_M68K_REG_PC,pc+2);boundaries.append(n)
     else:raise AssertionError((n,hex(pc)))
    u.hook_add(UC_HOOK_INTR,interrupt)
    dest=site+8 if skip or a4 else site-0x96
    u.emu_start(site-16,site+8 if skip else site,count=96)
    assert ccrvalue(u,site+8 if skip else site)==expectedccr
    u.reg_write(UC_M68K_REG_PC,site+8 if skip else site)
    u.ctl_remove_cache(site,site+8)
    if not skip:u.emu_start(site,dest,count=16)
    assert u.reg_read(UC_M68K_REG_PC)==dest and u.reg_read(UC_M68K_REG_A7)==stack,(ccr,d7,a4,error,hex(u.reg_read(UC_M68K_REG_PC)),hex(dest),hex(u.reg_read(UC_M68K_REG_A7)),boundaries)
    assert bytes(u.mem_read(site,8))==native
    if skip:
     assert boundaries==[32] and [u.reg_read(r) for r in regs]==expected
     assert ccrvalue(u,dest)==expectedccr
     assert bytes(u.mem_read(record,2))==b'\0\4'
    else:assert boundaries==[32,10] and u.reg_read(UC_M68K_REG_D1)==0x31415926
    cases+=1
print(f'PASS {cases} M68040 executions; native-prefix equivalence, original ABF7 PC/SP/registers/CCR, D7 paths, syscall error/success, both native tails')
