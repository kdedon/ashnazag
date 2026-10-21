from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'kernel/guest/mod/uinter/uipatch.c').read_text()
s=s[s.index('int uin_pollportdone;'):s.index('long uin_forcehits[')]
h='''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef char *caddr_t;
#define GR_A(r,n) (((long *)(r))[n])
#define G32(b) ((uint32_t)(b)[0]<<24 | (uint32_t)(b)[1]<<16 | (uint32_t)(b)[2]<<8 | (b)[3])
#define P32(b,v) do {uint32_t x=(v);(b)[0]=x>>24;(b)[1]=x>>16;(b)[2]=x>>8;(b)[3]=x;} while(0)
int uin_polldiag;
static unsigned char memory[8192];static int calls,faults;
static int copyin(caddr_t a,caddr_t b,int n) {
 uintptr_t p=(uintptr_t)a;calls++;
 if(p+n>sizeof memory){faults++;return 14;}
 memcpy(b,memory+p,n);return 0;
}
'''
t='''int main(void){
 long r[8]={0};int i,n;
 r[5]=2000;r[2]=6000;r[3]=6016;
 P32(memory+0x904,1900);P32(memory+1996,3000);P32(memory+2000,4000);
 P32(memory+4000,4500);P32(memory+0x1a5c,5000);
 for(i=0;i<64;i++){memory[5000+i]=i+1;memory[3000+i]=128+i;}
 for(i=0;i<32;i++)memory[6000+i]=i+64;
 for(i=0;i<64;i++)memory[4500+i]=i+192;
 forceport((char *)r);assert(!calls && !uin_pollportdone);
 uin_polldiag=1;forceport((char *)r);
 assert(uin_pollportdone && G32(uin_pollport)==2000 && G32(uin_pollport+4)==1900);
 assert(G32(uin_pollport+8)==3000 && G32(uin_pollport+12)==4000 && G32(uin_pollport+16)==4500);
 assert(G32(uin_pollport+20)==5000 && G32(uin_pollport+24)==6000 && G32(uin_pollport+28)==6016);
 assert(!memcmp(uin_pollport+32,memory+5000,64) && !memcmp(uin_pollport+96,memory+3000,64));
 assert(!memcmp(uin_pollport+160,memory+6000,32) && !faults);
 assert(!memcmp(uin_pollport+192,memory+4500,64));
 n=calls;forceport((char *)r);assert(calls==n);
 uin_pollportdone=0;memset(uin_pollport,0,sizeof uin_pollport);r[5]=100000;
 forceport((char *)r);assert(faults==1 && uin_pollportdone && !G32(uin_pollport+8));
 n=calls;forceport((char *)r);assert(calls==n);
 puts("forceport: 10 production checks passed");return 0;
}'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'t.c';o=Path(d)/'t';p.write_text(h+s+t)
 subprocess.run(['cc','-std=gnu89','-Wall','-Werror',str(p),'-o',str(o)],check=True)
 subprocess.run([str(o)],check=True)
