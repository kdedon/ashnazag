#!/usr/bin/env python3
import pathlib, subprocess, tempfile, sys
source=pathlib.Path(sys.argv[1]).read_text() if len(sys.argv)>1 else (pathlib.Path(__file__).resolve().parents[1]/'mod/uinter/uipatch.c').read_text()
a=source.index('#define MCOFFSPACE 26');b=source.index('#define MCOFFFRONT 21',a)
unit=source[a:b]
watch=source[source.index('if ((!uinter_patches && !spacewatch(gp))'):].split(' {',1)[0]
branch='int uinter_patches=1,uin_pmidle=0,uin_pmidlestub=0,pmstate=0; int ui_forceget(void){return 0;} int ui_forcepollowned(struct guest_proc *gp){return 0;} int pmwatchneeded(struct guest_proc *gp){return 0;} void completed(struct guest_proc *gp){int todo=0,coffwatch=0,coffbound=0;'+watch+' gp->gp_flags &= ~GPF_AWATCH;}\n'
head=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef char *caddr_t;
struct proc {caddr_t p_stkbase;int p_stksize;};
struct guest_proc {struct proc *gp_proc;int gp_flags;};
struct aux_proc {int ap_mac;};
struct rlimit {unsigned long rlim_cur;};
struct user {struct proc *u_procp;struct rlimit u_rlimit[4];} u;
struct uitask {struct guest_proc *t_gp;struct proc *t_proc;};
#define UI_NTASK 2
struct {struct guest_proc *l_gp,*l_active;struct proc *l_proc;struct uitask l_task[UI_NTASK];} ui;
struct proc parent,child,other;
struct guest_proc owner,joined,foreign;
struct aux_proc ap;
#define AUXP(gp) (&ap)
#define GUESTP(p) ((p)==&parent?&owner:&joined)
int ui_macrange(struct guest_proc *gp,unsigned long a,unsigned long n){return gp!=&foreign&&a>=0x3000&&!(a&1)&&a<0x2000000UL&&n<=0x2000000UL-a;}
#define APM_COFF 8
#define GPF_STUB 1
#define GPF_AWATCH 0x4000
#define RLIMIT_STACK 3
#define SIGSEGV 11
#define SIGKILL 9
#define GR_SR(r) (((long *)(r))[16])
#define GR_PC(r) (((long *)(r))[17])
#define GR_USP(r) (((long *)(r))[15])
#define GR_D(r,n) (((long *)(r))[n])
#define P16(p,v) do {((unsigned char *)(p))[0]=((unsigned long)(v)>>8)&255;((unsigned char *)(p))[1]=(v)&255;}while(0)
#define P32(p,v) do {P16(p,(unsigned long)(v)>>16);P16((char *)(p)+2,v);}while(0)
#define G16(p) ((unsigned long)((unsigned char *)(p))[0]*256+((unsigned char *)(p))[1])
#define G32(p) ((G16(p)<<16)|G16((char *)(p)+2))
unsigned char slot[4],stub[10],memerror[2];
int taskok=1,worldok=1,signals,owner_signals,allocs,failin,failout,partial;
int copyin(caddr_t address,caddr_t dst,int n) {
 uintptr_t a=(uintptr_t)address;
 if(failin && a==(uintptr_t)failin){failin=0;return 1;}
 if(a==0x594 && n==4){memcpy(dst,slot,4);return 0;}
 if(a==0x3000 && n==10){memcpy(dst,stub,10);return 0;}
 return 1;
}
int copyout(caddr_t src,caddr_t address,int n) {
 uintptr_t a=(uintptr_t)address;
 if(failout && a==(uintptr_t)failout){failout=0;if(partial && a==0x594)memcpy(slot,src,4);return 1;}
 if(a==0x220 && n==2){memcpy(memerror,src,2);return 0;}
 if(a==0x594 && n==4){memcpy(slot,src,4);return 0;}
 if(a==0x3000 && n==10){memcpy(stub,src,10);return 0;}
 return 1;
}
struct uitask *ui_task(struct guest_proc *gp){return !taskok ? 0 : gp==&owner ? &ui.l_task[0] : gp==&joined ? &ui.l_task[1] : 0;}
int ui_coffworld(struct guest_proc *gp,unsigned char *world){return worldok;}
void dlm_cacheflush(void){}
void psignal(struct proc *p,int sig){signals++;if(p==&parent)owner_signals++;}
int ui_auxcall(char *r,int sel,long arg,unsigned short *code,int mark,int ext) {
 assert(sel==0 && arg==0 && code[0]==0x203c && code[2]==10 && code[3]==0xa71e && mark==26 && ext==0);
 allocs++;return 1;
}
'''
tail=r'''
static int checks;
#define CHECK(x) do{assert(x);checks++;}while(0)
void reset(void){
 memset(&ui,0,sizeof ui);memset(&u,0,sizeof u);memset(slot,0,4);memset(stub,0,10);
 owner.gp_proc=&parent;joined.gp_proc=&child;foreign.gp_proc=&other;owner.gp_flags=joined.gp_flags=foreign.gp_flags=0;
 ui.l_gp=&owner;ui.l_proc=&parent;ui.l_task[0].t_gp=&owner;ui.l_task[0].t_proc=&parent;ui.l_task[1].t_gp=&joined;ui.l_task[1].t_proc=&child;u.u_procp=&child;ap.ap_mac=APM_COFF;
 child.p_stkbase=(caddr_t)(uintptr_t)0xc07ff000UL;child.p_stksize=0x1000;u.u_rlimit[3].rlim_cur=0x800000;
 spaceblock=spacepred=0;spacesession=spacepending=0;spaceproc=spacependingproc=0;spacestate=0;
 signals=owner_signals=allocs=failin=failout=partial=0;taskok=worldok=1;P32(slot,0x13e9c);
}
void installed(void){long r[18]={0};CHECK(spacestart(&joined,(char *)r)==1);spacefinish(&joined,0x3000,0);CHECK(spacevalid());}
int main(void){
 unsigned long n;long r[18]={0};struct proc bad;
 reset();CHECK(spacesize(&child,0xc07ff978,0x800000,&n));CHECK(n==0x7ff958 && n>0x928);
 CHECK(spacesize(&child,0xc07ff978,0x1000,&n)&&n==0x958);
 CHECK(spacesize(&child,0xc07ff000,0x1000,&n)&&n==0);
 CHECK(spacesize(&child,0xc07ff010,0x1000,&n)&&n==0);
 CHECK(spacesize(&child,0xc07ff030,0,&n)&&n==16);
 CHECK(!spacesize(&child,0xc0800000,0x800000,&n));
 CHECK(!spacesize(&child,0xc07fefff,0x800000,&n));
 CHECK(!spacesize(&child,0x2b7000,0x800000,&n));
 CHECK(!spacesize(&child,0xc07ff978,0x80000000,&n));
 bad=child;bad.p_stksize=-1;CHECK(!spacesize(&bad,0xc07ff978,0x800000,&n));
 bad.p_stksize=0;CHECK(!spacesize(&bad,0xc07ff978,0x800000,&n));
 bad.p_stksize=0x1000;bad.p_stkbase=(caddr_t)(uintptr_t)0xfffffff0UL;
 CHECK(!spacesize(&bad,0xfffffffa,0x800000,&n));
 installed();CHECK(owner.gp_flags&GPF_AWATCH);CHECK(joined.gp_flags&GPF_AWATCH);
 u.u_procp=&parent;completed(&owner);CHECK(owner.gp_flags&GPF_AWATCH);
 uinter_patches=0;completed(&owner);CHECK(owner.gp_flags&GPF_AWATCH);uinter_patches=1;
 ui.l_task[0].t_proc=&other;CHECK(!spacewatch(&owner));ui.l_task[0].t_proc=&parent;
 foreign.gp_flags=GPF_AWATCH;u.u_procp=&other;completed(&foreign);CHECK(!(foreign.gp_flags&GPF_AWATCH));u.u_procp=&parent;
 u.u_procp=&other;completed(&owner);CHECK(!(owner.gp_flags&GPF_AWATCH));
 u.u_procp=&parent;owner.gp_flags=GPF_AWATCH;taskok=0;completed(&owner);CHECK(!(owner.gp_flags&GPF_AWATCH));taskok=1;u.u_procp=&child;
 CHECK(allocs==1 && G32(slot)==0x3000 && G32(stub+6)==0x13e9c);
 CHECK(spacestart(&joined,(char *)r)==0 && allocs==1);
 r[17]=0x3000;r[15]=0xc07ff978;r[16]=0x1f;P16(memerror,23);
 CHECK(spacecall(&joined,(char *)r,1));CHECK(r[0]==0x7ff958 && r[17]==0x3002);CHECK(G16(memerror)==0 && r[16]==0x14);
 r[17]=0x3000;failout=0x220;r[0]=17;CHECK(spacecall(&joined,(char *)r,1)&&r[17]==0x3004&&r[0]==17);
 r[17]=0x3000;r[15]=0x2b7000;r[0]=17;
 CHECK(spacecall(&joined,(char *)r,1)&&r[17]==0x3004&&r[0]==17);
 r[17]=0x3000;r[15]=0xc07ff978;
 CHECK(spacecall(&joined,(char *)r,0)&&r[17]==0x3004);
 r[17]=0x3000;ap.ap_mac=0;CHECK(spacecall(&joined,(char *)r,1)&&r[17]==0x3004);ap.ap_mac=APM_COFF;
 r[17]=0x3000;taskok=0;CHECK(spacecall(&joined,(char *)r,1)&&r[17]==0x3004);taskok=1;
 r[17]=0x3000;u.u_procp=&other;CHECK(spacecall(&joined,(char *)r,1)&&r[17]==0x3004);u.u_procp=&child;
 CHECK(ui_stackspaceclear(&joined)==0&&spacevalid()&&G32(slot)==0x3000);
 CHECK(ui_stackspaceclear(&owner)==0&&G32(slot)==0x13e9c&&spacestate==0);
 reset();installed();P32(slot,0x5000);CHECK(ui_stackspaceclear(&owner)==0&&G32(slot)==0x5000&&spacestate==0);
 reset();installed();stub[2]^=1;CHECK(!spacevalid());r[17]=0x3000;CHECK(!spacecall(&joined,(char *)r,1));
 CHECK(ui_stackspaceclear(&owner)!=0);
 reset();CHECK(spacestart(&joined,(char *)r)==1);spacefinish(&joined,0x3000,1);CHECK(signals==1&&G32(slot)==0x13e9c);
 CHECK(ui_stackspaceclear(&joined)==0&&spacestate==0&&G32(slot)==0x13e9c);
 reset();CHECK(spacestart(&joined,(char *)r)==1);spacefinish(&joined,0x3001,0);CHECK(signals==1&&G32(slot)==0x13e9c);
 reset();CHECK(spacestart(&joined,(char *)r)==1);P32(slot,0x6000);spacefinish(&joined,0x3000,0);CHECK(signals==1&&G32(slot)==0x6000);
 reset();CHECK(spacestart(&joined,(char *)r)==1);failout=0x3000;spacefinish(&joined,0x3000,0);CHECK(signals==1&&G32(slot)==0x13e9c);
 reset();CHECK(spacestart(&joined,(char *)r)==1);failout=0x594;spacefinish(&joined,0x3000,0);CHECK(signals==1&&G32(slot)==0x13e9c);
 reset();CHECK(spacestart(&joined,(char *)r)==1);failout=0x594;partial=1;spacefinish(&joined,0x3000,0);CHECK(signals==1&&G32(slot)==0x13e9c);
 reset();CHECK(spacestart(&joined,(char *)r)==1);worldok=0;spacefinish(&joined,0x3000,0);CHECK(signals==1&&G32(slot)==0x13e9c);
 reset();installed();failout=0x594;CHECK(ui_stackspaceclear(&owner)!=0&&spacestate==0);
 reset();installed();ui.l_proc=&other;CHECK(!spacevalid());
 reset();P32(slot,0x5001);CHECK(spacestart(&joined,(char *)r)==-1&&allocs==0);
 reset();P32(slot,0x40900000);CHECK(spacestart(&joined,(char *)r)==-1&&allocs==0);
 reset();P32(slot,0x40801234);CHECK(spacestart(&joined,(char *)r)==1);spacefinish(&joined,0x3000,0);CHECK(spacevalid()&&G32(stub+6)==0x40801234);
 printf("%d owned StackSpace source checks PASS\n",checks);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='coff-space-') as d:
 p=pathlib.Path(d);(p/'test.c').write_text(head+unit+branch+tail)
 subprocess.run(['cc','-std=gnu89','-O1','-fsanitize=undefined','-fno-sanitize-recover=all','-Wno-pointer-sign',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
