#!/usr/bin/env python3
"""Exercise live-owner native entry restoration and closing-session admission."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'mod/uinter/uipatch.c').read_text()
def block(at):
 a=s.index(at);j=s.index('{',a);depth=1;k=j+1
 while depth:
  depth+=(s[k]=='{')-(s[k]=='}');k+=1
 return s[a:k]
body=block('static void\npmexpected(')+s[s.index('long uin_pmidleclearerror;'):s.index('\n#define MCOFFSPACE')]
header=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef char *caddr_t;
struct proc {int id;};struct guest_proc {struct proc *gp_proc;};
struct {struct proc *u_procp;} u;
static struct guest_proc *pmowner;static struct proc *pmproc;
static long pmblock,pmsite,pmsys;static int pmstate,writes,reads,fault,writefault,flushes;
static long uin_pmidlerestores;
static unsigned char pmnative[8]={0xab,0xf7,0x20,0x0c,0x67,0,0xff,0x64};
static unsigned char pmprefix[16]={0x0c,0x47,0,1,0x66,0x0a,0x28,0x6d,0xfb,0xaa,0x38,0xbc,0,4,0x60,8};
static unsigned char pmcode[60],memory[0x10000];
#define P16(p,v) ((p)[0]=((v)>>8)&255,(p)[1]=(v)&255)
#define P32(p,v) (P16(p,(v)>>16),P16((p)+2,v))
int copyin(caddr_t a,caddr_t b,int n){unsigned long p=(unsigned long)a;reads++;if(fault||p+n>sizeof memory)return 1;memcpy(b,memory+p,n);return 0;}
int copyout(caddr_t a,caddr_t b,int n){unsigned long p=(unsigned long)b;writes++;if(writefault||p+n>sizeof memory)return 1;memcpy(memory+p,a,n);return 0;}
void dlm_cacheflush(void){flushes++;}
static int checks;
#define OK(x) do{assert(x);checks++;}while(0)
'''
test=r'''
static void install(struct guest_proc *g){unsigned char code[60],redirect[16];pmowner=g;pmproc=g->gp_proc;pmstate=2;pmblock=0x5000;pmsite=0x2000;pmsys=0x3000;fault=writefault=0;pmexpected(code,redirect);memcpy(memory+pmsite-16,redirect,16);memcpy(memory+pmsite,pmnative,8);memcpy(memory+pmblock,code,60);}
int main(void){struct proc p={1},other={2};struct guest_proc gp={&p},foreign={&other};int before;
 u.u_procp=&p;install(&gp);before=writes;OK(!ui_pmidleclear(&foreign)&&pmowner==&gp&&writes==before);
 u.u_procp=&other;before=reads;OK(ui_pmidleclear(&gp)==1&&pmowner==&gp&&pmstate==2&&reads==before);u.u_procp=&p;
 memset(memory+pmblock,0,60);before=writes;OK(!ui_pmidleclose(&gp)&&pmstate==3&&pmowner==&gp&&writes==before+1&&uin_pmidlerestores==1);
 OK(!memcmp(memory+0x2000,pmnative,8)&&!memcmp(memory+0x1ff0,pmprefix,16));before=writes;OK(!ui_pmidleclose(&gp)&&pmstate==3&&writes==before);
 OK(!ui_pmidleclear(&gp)&&!pmstate&&!pmowner&&writes==before);
 install(&gp);memory[pmsite+4]^=1;before=writes;OK(ui_pmidleclear(&gp)==3&&writes==before&&pmowner==&gp&&pmstate==2);
 ui_pmidleforget(&foreign);OK(pmowner==&gp);ui_pmidleforget(&gp);OK(!pmowner&&!pmstate);
 install(&gp);fault=1;before=writes;OK(ui_pmidleclose(&gp)==2&&writes==before&&pmowner==&gp&&pmstate==2);fault=0;
 writefault=1;OK(ui_pmidleclose(&gp)==4&&pmowner==&gp&&pmstate==2);writefault=0;OK(!ui_pmidleclose(&gp)&&pmstate==3&&uin_pmidlerestores==2);
 ui_pmidleclear(&gp);install(&gp);memcpy(memory+pmsite-16,pmprefix,16);before=writes;OK(!ui_pmidleclose(&gp)&&pmstate==3&&writes==before&&uin_pmidlerestores==2);
 OK(!ui_pmidleclear(&gp)&&!pmstate&&!pmowner);
 printf("pmclose: %d production cases PASS\n",checks);
}
'''
# Closing tasks must neither reserve another stub nor enter fallback polling.
assert '(pmstate==3 && pmowner==gp)' in block('static int\npmidleowned(')
assert 'pmwatchneeded(gp)' in block('void\nui_fronttick(')
assert 'pmstate==3 || (uin_pmidlestub && pmstate==2)' in block('static int\npmwatchneeded(')
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(header+body+test)
 subprocess.run(['cc','-std=gnu89','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
