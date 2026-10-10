#!/usr/bin/env python3
import pathlib,re,subprocess,sys,tempfile
s=pathlib.Path(sys.argv[1]).read_text()
a=s.index('long uin_wnestats[4]');z=s.index('\nstatic int\nfrontcapture',a);f=s[a:z]
cap=s[s.index('static int\nfrontcapture'):s.index('\nstatic int\npollsites')]
assert cap.index('G32(b+10)!=w+8')<cap.index('wnecapture(ptr)')<cap.index('G32(rec)!=G32(rec+8)')
front=s[s.index('static unsigned char frontentry[]'):s.index('#define FRONTEXT')]
front=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})\b',front))
assert front[:2]==bytes.fromhex('42e7') and front[14:18]==bytes.fromhex('4e56fee8')
assert bytes.fromhex('486efee8') in front
# MOVEM15 registers60 + BSRreturn4 + savedCCR2 + LINK4 + locals280.
w=s[s.index('static unsigned char wneentry[]'):s.index('#define POLLUP')]
v=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})\b',w))
assert v[:8]==bytes.fromhex('48e7fffe610001f8')
assert 60+4+2+4+280==350
p=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef char *caddr_t;
unsigned char memory[0x1000000];int fault,reads;
#define G16(p) ((unsigned long)((unsigned char*)(p))[0]<<8|((unsigned char*)(p))[1])
#define G32(p) (G16(p)<<16|G16((p)+2))
#define P16(p,v) ((p)[0]=((v)>>8)&255,(p)[1]=(v)&255)
#define P32(p,v) (P16((p),(v)>>16),P16((p)+2,(v)))
int copyin(caddr_t a,caddr_t b,unsigned long n){unsigned long p=(uintptr_t)a;reads++;assert(n==20&&p+n<=sizeof memory);if(fault)return 1;memcpy(b,memory+p,n);return 0;}
'''
t=r'''
int main(void){unsigned char *p=memory+0x5000+350;int i;
P32(p,0x7fb4e0);P32(p+4,0x7000);P32(p+12,0x8000);P16(p+16,0xffff);P16(p+18,0);
for(i=0;i<3;i++){P32(p+8,i==0?0:i==1?60:0xffffffffUL);wnecapture(0x5000);assert(uin_wnestats[0]==i+1&&uin_wnestats[i+1]==1);assert(uin_wnelast[0]==0x7fb4e0&&uin_wnelast[1]==0xffff&&uin_wnelast[2]==G32(p+8)&&uin_wnelast[3]==0x7000&&uin_wnelast[4]==0x8000&&uin_wnelast[5]==0x5000+350);}
fault=1;wnecapture(0x5000);assert(uin_wnestats[0]==3);fault=0;
i=reads;wnecapture(-1);wnecapture(0x2fff);wnecapture(0x1000000-369);assert(reads==i&&uin_wnestats[0]==3);
wnecapture(0x1000000-370);assert(uin_wnestats[0]==4);
puts("idle-wne: 8 production capture cases and assembled stack oracle PASS");return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+t);o=pathlib.Path(d)/'t'
 subprocess.run(['cc','-std=gnu89','-fsanitize=undefined','-Werror',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
