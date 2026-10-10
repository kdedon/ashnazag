#!/usr/bin/env python3
"""Compile the production installer with faultable user-memory fixtures."""
from pathlib import Path
import subprocess,sys
source=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
production=source.read_text();production="\n".join(x for x in production.splitlines() if not x.startswith("#include"))
prelude=r'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define SIGSEGV 11
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define LS_INUSE 2
typedef char *caddr_t;
struct proc {void *p_as;};
struct guest_proc {struct proc *gp_proc;};
static struct proc process;
static struct guest_proc guest={&process},foreign={&process};
static struct {struct proc *u_procp;} u={&process};
static struct {struct guest_proc *l_gp,*l_active;int l_state;} ui={&guest,&guest,2};
static unsigned char memory[65536];
static int reads,writes,readfail,writefail,deny,signals,flushes,partial,checks;
static unsigned long G32(const void *v) {const unsigned char *p=v;return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];}
static unsigned short G16(const void *v) {const unsigned char *p=v;return (p[0]<<8)|p[1];}
static void P32(void *v,unsigned long n) {unsigned char *p=v;p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static void P16(void *v,unsigned short n) {unsigned char *p=v;p[0]=n>>8;p[1]=n;}
static int copyin(caddr_t a,caddr_t b,long n) {unsigned long x=(unsigned long)a;++reads;if(reads==readfail || x+n>sizeof memory)return 1;memcpy(b,memory+x,n);return 0;}
static int copyout(caddr_t a,caddr_t b,long n) {unsigned long x=(unsigned long)b;++writes;if(x+n>sizeof memory)return 1;if(writefail&(1<<writes)){if(partial)memcpy(memory+x,a,n/2);return 1;}memcpy(memory+x,a,n);return 0;}
static int as_checkprot(void *as,caddr_t a,long n,int prot) {(void)as;(void)a;(void)n;(void)prot;return deny;}
static int bcmp(const void *a,const void *b,long n){return memcmp(a,b,n);}
static void psignal(struct proc *p,int s){assert(p==&process && s==11);++signals;}
void dlm_cacheflush(void){++flushes;}
#define C(x) do {assert(x);++checks;} while(0)
'''
tests=r'''
static char request[20];
static void reset(void)
{
    memset(memory,0,sizeof memory);routeowner=0;routepc=routetail=routehelper=routelo=routehi=0;
    reads=writes=readfail=writefail=deny=signals=flushes=partial=0;
    ui.l_gp=ui.l_active=&guest;ui.l_state=2;u.u_procp=&process;
    memcpy(memory+0x2000,routenative,sizeof routenative);
    memcpy(memory+0x4000,routestub,sizeof routestub);
    P32(memory+0x400c,0x4026);
    P16(memory+0x4026,0x4ef9);P32(memory+0x4028,0x4100);
    P32(memory+0x402c,0x4f544144);P16(memory+0x4030,1);
    P16(memory+0x15a,0x0810);
    P32(request,0x2000);P32(request+4,0x4000);P32(request+8,0x4026);
    P32(request+12,0x4000);P32(request+16,0x1000);
}
int main(void)
{
    int i,maxreads;unsigned char saved[28];
    reset();C(!ui_otroute(&guest,request));C(routeowner==&guest);C(G32(memory+0x4022)==0x2008);
    C(G16(memory+0x2000)==0x4ef9 && G32(memory+0x2002)==0x4000);C(writes==2);
    C(!ui_otroute(&guest,request));C(writes==2);C(!ui_otrouteclear(&guest));
    C(!memcmp(memory+0x2000,routenative,sizeof routenative) && !routeowner);
    reset();C(ui_otroute(&foreign,request)==EPERM && !writes);
    reset();ui.l_active=&foreign;C(ui_otroute(&guest,request)==EPERM && !writes);
    reset();ui.l_state=0;C(ui_otroute(&guest,request)==EPERM && !writes);
    reset();P16(memory+0x15a,0x0760);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();P32(request+16,0x40001);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();P32(request+12,0x7ffffffe);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();deny=1;C(ui_otroute(&guest,request)==EINVAL && !writes);
    for(i=0;i<28;i++) {reset();memory[0x2000+i]^=1;C(ui_otroute(&guest,request)==EINVAL && !writes);}
    for(i=0;i<38;i++) {if((i>=12 && i<16)||(i>=34 && i<38))continue;reset();memory[0x4000+i]^=1;C(ui_otroute(&guest,request)==EINVAL && !writes);}
    reset();P32(memory+0x400c,0x4028);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();P32(memory+0x4022,0x200a);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();P32(memory+0x4028,0x6000);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();P32(memory+0x4028,0x4000);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();P32(memory+0x402c,0);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();P16(memory+0x4030,2);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();P32(memory+0x4032,1);C(ui_otroute(&guest,request)==EINVAL && !writes);
    reset();C(!ui_otroute(&guest,request));maxreads=reads;
    for(i=1;i<=maxreads;i++){reset();readfail=i;C(ui_otroute(&guest,request)!=0 && !routeowner);C(!memcmp(memory+0x2000,routenative,28));}
    reset();writefail=2;partial=1;C(ui_otroute(&guest,request)==EFAULT);C(!routeowner && !memcmp(memory+0x2000,routenative,28));
    reset();writefail=4;partial=1;C(ui_otroute(&guest,request)==EFAULT);C(!routeowner && !memcmp(memory+0x2000,routenative,28));
    reset();writefail=12;partial=1;C(ui_otroute(&guest,request)==EFAULT);C(routeowner==&guest && signals==1);
    reset();C(!ui_otroute(&guest,request));memcpy(saved,memory+0x2000,28);memory[0x2008]^=1;memcpy(saved,memory+0x2000,28);i=writes;
    C(ui_otrouteclear(&guest)==EFAULT && signals==1 && routeowner==&guest);C(writes==i && !memcmp(saved,memory+0x2000,28));
    reset();C(!ui_otroute(&guest,request));memory[0x4000]^=1;i=writes;C(ui_otrouteclear(&guest)==EFAULT && writes==i && routeowner);
    reset();C(!ui_otroute(&guest,request));C(!ui_otrouteclear(&foreign) && routeowner==&guest);
    reset();C(!ui_otroute(&guest,request));writefail=1<<(writes+1);C(ui_otrouteclear(&guest)==EFAULT && signals==1 && routeowner);
    reset();C(!ui_otroute(&guest,request));P32(request+4,0x4002);C(ui_otroute(&guest,request)==EBUSY);
    reset();{char context[8];P32(context,0x6000);P32(context+4,99);P32(memory+0x6000,0x7000);P32(memory+0x7040,0x8000);P32(memory+0x7044,0x9000);
    C(!ui_otroutecontext(&guest,context) && !G32(context+4));C(!writes);
    C(!ui_otroute(&guest,request));C(!ui_otroutecontext(&guest,context) && G32(context+4)==1);
    P32(memory+0x7040,0x8001);C(ui_otroutecontext(&guest,context)==EINVAL);P32(memory+0x7040,0x8000);
    deny=1;C(ui_otroutecontext(&guest,context)==EINVAL);deny=0;
    C(ui_otroutecontext(&foreign,context)==EPERM);
    P32(memory+0x6000,0);C(ui_otroutecontext(&guest,context)==EINVAL);}
    printf("%d route installer checks PASS\n",checks);return 0;
}
'''
c=out/"route-host.c";c.write_text(prelude+production+tests)
subprocess.run(["nice","-n19","cc","-std=c99","-Wall","-Wextra","-Werror","-Wno-sign-compare","-fsanitize=address,undefined",str(c),"-o",str(out/"route-host")],check=True)
subprocess.run([str(out/"route-host")],check=True,env={"ASAN_OPTIONS":"detect_leaks=0"})
