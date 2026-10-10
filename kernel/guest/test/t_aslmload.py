from pathlib import Path
import re,subprocess,sys,os,tempfile
temporary=tempfile.TemporaryDirectory(prefix='aslm-load-') if len(sys.argv)<=2 else None
p=Path(sys.argv[2]) if len(sys.argv)>2 else Path(temporary.name)
p.mkdir(parents=True,exist_ok=True)
s=Path(sys.argv[1]).read_text() if len(sys.argv)>1 else (Path(__file__).resolve().parents[1]/'mod/uinter/uipatch.c').read_text()
marks=re.findall(r'^#define\s+(M[A-Z0-9_]+)\s+(\d+)(?=\s|$)',s,re.M)
assert len({int(value) for name,value in marks})==len(marks),marks
print('PASS all production frame marks have distinct values',flush=True)
auxstart=s.index('static int\nui_auxcall(')
aux=s[auxstart:s.index('\n}\n',auxstart)+3]
helpers=s[s.index('static unsigned char aslmclass[]'):s.index('static void\naslmresult(')]
a=s.index('    if(G16(b+18)==MCLASS');z=s.index('    if(G16(b+18)==MASLM)',a)
resume=s[a:z]
preamble=r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef char *caddr_t;
typedef int32_t regword __attribute__((aligned(1),may_alias));
#define GR_USP(r) (*(regword *)((r)+0))
#define GR_PC(r) (*(regword *)((r)+66))
#define GR_D(r,n) (*(regword *)((r)+4+4*(n)))
#define GR_A(r,n) (*(regword *)((r)+36+4*(n)))
#define GR_SR(r) (*(uint16_t *)((r)+64))
#define FSIZE 96
#define RESUME 0xa89f
#define FMAGIC 0x75697063
#define MCLASS 15
#define MREGEXIT 16
#define MLOAD 17
#define MLOADEXIT 18
#define MGET 19
#define MMPSLOAD 20
#define MGETEXIT 22
#define MGINSTALL 24
#define MCOFFFRONT 21
#define MCOFFAUXFRONT 23
#define MPATH 13
#define GPF_STUB 0x80000000U
#define SIGSEGV 11
#define G16(p) (((uint32_t)(p)[0]<<8)|(p)[1])
#define G32(p) ((G16(p)<<16)|G16((p)+2))
#define P16(p,v) do {(p)[0]=(unsigned char)((v)>>8);(p)[1]=(unsigned char)(v);}while(0)
#define P32(p,v) do {P16(p,(uint32_t)(v)>>16);P16((p)+2,(v));}while(0)
static unsigned char mem[0x100000];
static unsigned int copylimit=0x20000;
static int failed,signalled,uinter_aslmtrace=1,flushes,reads;
static int32_t uin_aslmreads,uin_aslmcode;
struct guest_proc {void *gp_proc;uint32_t gp_flags;};
static struct {void *u_procp;} u;
#define GUESTP(p) ((struct guest_proc *)(p))
static int32_t uin_coffrefused;
static int uinter_coffnative47;static int32_t uin_coff47[16];
static int ui_coffworld(struct guest_proc *gp,char *b) {(void)gp;(void)b;return 0;}
struct proc {int p_pid;};
static struct {struct guest_proc *l_gp;struct proc *l_proc;int l_state;} ui;
static struct guest_proc *coffsyserrparent;
static struct proc *coffsyserrproc;
static int32_t uin_coffsyserrparentpid;
static unsigned short coffsyserrversion;
static void psignal(void *p,int sig) {(void)p;assert(sig==11);signalled++;}
static int copyin(caddr_t p,caddr_t b,int n) {
 uintptr_t a=(uintptr_t)p;reads++;if(a+n>copylimit)return 1;memcpy(b,mem+a,n);return 0;
}
static int copyout(caddr_t b,caddr_t p,int n) {
 uintptr_t a=(uintptr_t)p;if(a+n>copylimit||a==failed)return 1;memcpy(mem+a,b,n);return 0;
}
static void dlm_cacheflush(void) {flushes++;}
#define bcopy model_bcopy
static void model_bcopy(const char *a,char *b,int n) {memcpy(b,a,n);}
'''
code=preamble+re.sub(r'\blong\b','int32_t',aux+helpers)
code+='static int finish(char *r,int32_t f) {unsigned char b[FSIZE];memcpy(b,mem+f,FSIZE);'+re.sub(r'\blong\b','int32_t',resume)+'return 0;}\n'
code+=r'''
static void reset(struct guest_proc *gp) {
 memset(mem,0,sizeof(mem));copylimit=0x20000;failed=signalled=0;aslmowner=0;aslmclasspc=aslmexitpc=0;
 memcpy(mem+0xf958,aslmget,16);memcpy(mem+0xfaa8,aslmgetexit,16);aslmmpcount=0;
 memcpy(mem+0xf572,aslmload,16);memcpy(mem+0xf646,aslmloadexit,16);
 memcpy(mem+0xdaae,aslmindexedreturn,16);memcpy(mem+0x12c28,aslmcaller,16);memcpy(mem+0x12912,aslmclass,16);memcpy(mem+0x12c78,aslmexit,16);
 gp->gp_proc=gp;gp->gp_flags=0;u.u_procp=gp;ui.l_gp=gp;
}
int main(void) {
 struct guest_proc gp,foreign;char r[72],post[72];int prior,mark,i;int32_t f,pc,sp=0x1e000,fp=0x1c000;
 reset(&gp);P32(mem+fp,fp+28);P32(mem+fp+32,0x12c28);P32(mem+fp+36,0x2a3594);
 assert(aslmparent(fp,0xdaae,0x2a3594)==0x12c28);
 assert(!aslmparent(fp,0xdaae,0x2a3598));
 P32(mem+fp,fp+32);assert(!aslmparent(fp,0xdaae,0x2a3594));P32(mem+fp,fp+28);
 P32(mem+fp+32,0x12c2c);assert(!aslmparent(fp,0xdaae,0x2a3594));P32(mem+fp+32,0x12c28);
 mem[0xdaae]^=1;assert(!aslmparent(fp,0xdaae,0x2a3594));mem[0xdaae]^=1;
 assert(!aslmparent(0x7fffffd8,0xdaae,0x2a3594));
 reset(&gp);assert(!aslmparent(fp,0x1fffc,0x2a3594));
 assert(!aslmparent(0x1fffe,0xdaae,0x2a3594));
 P32(mem+0x1fff0,0x2000c);assert(!aslmparent(0x1fff0,0xdaae,0x2a3594));
 memcpy(mem+0x1d000,aslmindexedreturn,16);P32(mem+fp,fp+28);
 P32(mem+fp+32,0x1d000 - 0xdaae + 0x12c28);P32(mem+fp+36,0x2a3594);
 assert(!aslmparent(fp,0x1d000,0x2a3594));
 reset(&gp);P32(mem+fp,fp+28);P32(mem+fp+32,0x12c28);P32(mem+fp+36,0x2a3594);
 mem[0x12c28]^=1;assert(!aslmparent(fp,0xdaae,0x2a3594));
 assert(aslmfailseen==0x3fe);
 for(prior=0;prior<2;prior++)for(mark=MCLASS;mark<=MREGEXIT;mark++) {
  reset(&gp);aslminstall(0x12c28);assert(aslmowner==&gp);
  assert(G16(mem+0x12912)==RESUME&&G16(mem+0x12c78)==RESUME);
  memset(r,0,sizeof(r));for(i=4;i<64;i++)r[i]=i;GR_A(r,6)=fp;
  pc=mark==MCLASS?0x12912:0x12c78;GR_PC(r)=pc;GR_USP(r)=sp;GR_SR(r)=0x271f;
  gp.gp_flags=prior?GPF_STUB:0;P32(mem+fp+8,0x2a3594);P32(mem+fp-60,0x1b000);
  GR_D(r,6)=14;P32(mem+fp-60,0x1b000-14);
  P16(mem+fp-56,1);P32(mem+0x1b000,4);P16(mem+0x1b004,0x311);P16(mem+0x1b006,0x311);
  memcpy(mem+0x1b008,"OTModl$auxtcp",13);P16(mem+fp-142,0xf3bb);
  assert(aslmstep(&gp,r));f=GR_USP(r);assert(f==sp-FSIZE);assert(G16(mem+f+18)==mark);
  assert(G16(mem+f+90)==prior);assert(G16(mem+f+4)==G16(mark==MCLASS?aslmclass:aslmexit));
  assert(gp.gp_flags&GPF_STUB);
  /* Model the displaced instruction; every other register must survive. */
  if(mark==MCLASS)P32(mem+fp-60,G32(mem+fp-60)+GR_D(r,6));
  if(mark==MREGEXIT)GR_D(r,0)=(GR_D(r,0)&~0xffff)|G16(mem+fp-142);
  GR_SR(r)=0x2710;GR_PC(r)=f+16;memcpy(post,r,sizeof(r));
  assert(finish(r,f)==(prior?2:1));assert(GR_PC(r)==pc+4&&GR_USP(r)==sp);
  assert(!memcmp(r+4,post+4,62));ui_aslmclear(&foreign);assert(aslmowner==&gp);
  ui_aslmclear(&gp);assert(!aslmowner);assert(G16(mem+0x12912)==0xddae&&G16(mem+0x12c78)==0x302e);
 }
 reset(&gp);mem[0x1291f]^=1;aslminstall(0x12c28);assert(!aslmowner&&G16(mem+0x12c78)==0x302e);
 reset(&gp);failed=0x12c78;aslminstall(0x12c28);assert(!aslmowner&&G16(mem+0x12912)==0xddae);
 reset(&gp);aslminstall(0x12c28);memset(r,0,sizeof(r));GR_USP(r)=sp;GR_PC(r)=0x12912;
 failed=sp-FSIZE;assert(aslmstep(&gp,r));assert(GR_USP(r)==sp&&GR_PC(r)==0x12912);
 assert(G16(mem+0x12912)==0xddae);failed=0;ui_aslmclear(&gp);
 reset(&gp);aslminstall(0x12c28);GR_USP(r)=sp;GR_PC(r)=0x12c78;failed=sp-FSIZE+90;
 assert(aslmstep(&gp,r));assert(GR_USP(r)==sp&&GR_PC(r)==0x12c78&&G16(mem+0x12c78)==0x302e);
 failed=0;ui_aslmclear(&gp);
 reset(&gp);aslminstall(0x12c28);mem[0x1291f]^=1;GR_USP(r)=sp;GR_PC(r)=0x12912;
 assert(aslmstep(&gp,r)&&signalled==1);assert(G16(mem+0x12912)==RESUME);
 ui_aslmclear(&gp);assert(G16(mem+0x12912)==RESUME);assert(G16(mem+0x12c78)==0x302e);

 for(prior=0;prior<2;prior++)for(mark=MLOAD;mark<=MLOADEXIT;mark++) {
  reset(&gp);aslminstall(0x12c28);assert(aslmowner==&gp);
  assert(G16(mem+0xf572)==RESUME&&G16(mem+0xf646)==RESUME);
  memset(r,0,sizeof(r));for(i=4;i<64;i++)r[i]=i;GR_A(r,6)=fp;
  pc=mark==MLOAD?0xf572:0xf646;GR_PC(r)=pc;GR_USP(r)=sp;GR_SR(r)=0x271f;
  gp.gp_flags=prior?GPF_STUB:0;
  P32(mem+fp+8,0x244308);P32(mem+fp+12,0x1b000);mem[fp+19]=1;
  memcpy(mem+0x1b000,"OTModl$auxtcp",13);GR_D(r,4)=0x1234f3d0;
  assert(aslmstep(&gp,r));f=GR_USP(r);assert(f==sp-FSIZE);
  assert(G16(mem+f+18)==mark&&G16(mem+f+90)==prior);
  assert(G16(mem+f+4)==G16(mark==MLOAD?aslmload:aslmloadexit)&&G16(mem+f+6)==0x4e71);
  if(mark==MLOAD)GR_D(r,3)=0;
  else GR_D(r,0)=(GR_D(r,0)&~0xffff)|(GR_D(r,4)&0xffff);
  GR_SR(r)=0x2718;GR_PC(r)=f+16;memcpy(post,r,sizeof(r));
  assert(finish(r,f)==(prior?2:1));assert(GR_PC(r)==pc+2&&GR_USP(r)==sp);
  assert(!memcmp(r+4,post+4,62));ui_aslmclear(&gp);
  assert(G16(mem+0xf572)==0x7600&&G16(mem+0xf646)==0x3004);
 }
 for(i=0;i<4;i++) {
  reset(&gp);failed=(i==0?0x12912:i==1?0x12c78:i==2?0xf572:0xf646);
  aslminstall(0x12c28);assert(!aslmowner);
  assert(G16(mem+0x12912)==0xddae&&G16(mem+0x12c78)==0x302e);
  assert(G16(mem+0xf572)==0x7600&&G16(mem+0xf646)==0x3004);
 }
 reset(&gp);mem[0xf57f]^=1;aslminstall(0x12c28);assert(!aslmowner);
 reset(&gp);mem[0xf653]^=1;aslminstall(0x12c28);assert(!aslmowner);
 reset(&gp);aslminstall(0x12c28);uin_aslmreads=99;failed=0xf572;
 ui_aslmclear(&gp);assert(!aslmowner&&uin_aslmreads==99&&G16(mem+0xf572)==RESUME);
 puts("PASS class-load two-word continuations, nested state, native error/CCR/register preservation, all four write rollbacks and guarded teardown");
 reset(&gp);copylimit=sizeof mem;aslminstall(0x12c28);assert(G16(mem+0xf958)==RESUME);
 memcpy(mem+0x55716,aslmmpcall,16);memcpy(mem+0x55722,aslmmpreturn,16);
 memcpy(mem+0x306d8,aslmmp,16);memcpy(mem+0x309d8,aslmmpinstall,16);memcpy(mem+0x55794,"\x20\x39",2);
 P32(mem+0x55796,0x70000);P16(mem+0x5579a,0x4e75);P16(mem+0x5579c,0x2039);P16(mem+0x557a2,0x660e);
 P32(mem+0x70000,0x71000);P32(mem+0x71000,0x72000);P32(mem+0x72040,0xf566);
 P32(mem+fp,fp+28);P32(mem+fp+32,0x55722);GR_A(r,6)=fp;
 aslmmpsinstall(r);assert(aslmmpcount==1&&aslmmpcopies[0].pc==0x306d8);
 assert(G16(mem+0x306d8)==RESUME);aslmmpsinstall(r);assert(aslmmpcount==1);
 for(prior=0;prior<2;prior++)for(mark=MGET;mark<=MMPSLOAD;mark++) {
  memset(r,0,sizeof(r));for(i=4;i<64;i++)r[i]=i;GR_A(r,6)=fp;
  pc=mark==MGET?0xf958:0x306d8;GR_PC(r)=pc;GR_USP(r)=sp;GR_SR(r)=0x271f;
  gp.gp_flags=prior?GPF_STUB:0;P32(mem+fp+8,0x71000);P32(mem+fp+12,0x1b000);P32(mem+fp+24,0);
  memcpy(mem+0x1b000,"OTModl$auxtcp",13);
  assert(aslmstep(&gp,r));f=GR_USP(r);assert(G16(mem+f+18)==mark);
  if(mark==MGET)GR_A(r,4)=0x71000;else GR_A(r,2)=0;
  GR_PC(r)=f+16;GR_SR(r)=0x271f;memcpy(post,r,sizeof(r));
  assert(finish(r,f)==(prior?2:1));assert(GR_PC(r)==pc+4&&GR_USP(r)==sp);
  assert(!memcmp(r+4,post+4,62));
 }
 for(prior=0;prior<2;prior++) {
  memset(r,0,sizeof r);GR_A(r,6)=fp;GR_PC(r)=0x309d8;GR_USP(r)=sp;GR_D(r,0)=0x19000;
  P32(mem+fp+8,0x1a000);strcpy((char *)mem+0x1a000,"auxtcp");gp.gp_flags=prior?GPF_STUB:0;
  assert(aslmstep(&gp,r));f=GR_USP(r);assert(G16(mem+f+18)==MGINSTALL&&G16(mem+f+6)==0x4e71);
  GR_A(r,2)=GR_D(r,0);GR_SR(r)=0x2000;memcpy(post,r,sizeof post);
  assert(finish(r,f)==(prior?2:1));assert(GR_PC(r)==0x309da&&GR_USP(r)==sp);
  assert(!memcmp(r+4,post+4,62)&&aslminstallreturns==prior+1);
 }
 strcpy((char *)mem+0x1a000,"foreign");aslmsteplog(r,MGINSTALL,0x309d8);assert(aslminstallreturns==2);
 for(i=0;i<20;i++) {strcpy((char *)mem+0x1a000,"auxudp");aslmsteplog(r,MGINSTALL,0x309d8);}
 assert(aslminstallreturns==16);
 ui_aslmclear(&gp);assert(!aslmmpcount&&G16(mem+0x306d8)==0x246e&&G16(mem+0xf958)==0x286e);
 assert(G16(mem+0x309d8)==0x2440&&!aslminstallreturns);
 assert(!aslmfield(0x7fffffff,64)&&!aslmfield(-1,8)&&!aslmfield(0,-1));
 puts("PASS checked MPS callsite/cache chain, duplicate copy refusal, native four-byte frame replay, getter return instruction/CCR preservation, own-name cap and teardown");
 for(i=0;i<7;i++) {
  reset(&gp);copylimit=sizeof mem;aslminstall(0x12c28);
  memcpy(mem+0x55716,aslmmpcall,16);memcpy(mem+0x55722,aslmmpreturn,16);
  memcpy(mem+0x306d8,aslmmp,16);memcpy(mem+0x309d8,aslmmpinstall,16);memcpy(mem+0x55794,"\x20\x39",2);
  P32(mem+0x55796,0x70000);P16(mem+0x5579a,0x4e75);P16(mem+0x5579c,0x2039);P16(mem+0x557a2,0x660e);
  P32(mem+0x70000,0x71000);P32(mem+0x71000,0x72000);P32(mem+0x72040,0x80000);
  memcpy(mem+0x8000c,aslmload,16);memcpy(mem+0x800e0,aslmloadexit,16);
  P32(mem+fp,fp+28);P32(mem+fp+32,0x55722);GR_A(r,6)=fp;
  if(i==1)failed=0x306d8;
  if(i==2)failed=0x8000c;
  if(i==3)failed=0x800e0;
  if(i==4)mem[0x55716]^=1;
  if(i==5)P32(mem+fp,fp+32);
  if(i==6)failed=0x309d8;
  aslmmpsinstall(r);
  if(i==0) {
   assert(aslmmpcount==1&&aslmmpcopies[0].loadpc==0x8000c&&aslmmpcopies[0].exitpc==0x800e0);
   assert(G16(mem+0x306d8)==RESUME&&G16(mem+0x8000c)==RESUME&&G16(mem+0x800e0)==RESUME);
  } else {
   assert(!aslmmpcount&&G16(mem+0x306d8)==0x246e&&G16(mem+0x8000c)==0x7600&&G16(mem+0x800e0)==0x3004);
  }
  failed=0;ui_aslmclear(&gp);
  assert(G16(mem+0x306d8)==0x246e&&G16(mem+0x8000c)==0x7600&&G16(mem+0x800e0)==0x3004);
 }
 puts("PASS separate class-method copy, four installation-write rollbacks, caller-signature and frame-chain refusals");
 reset(&gp);aslminstall(0x12c28);memset(r,0,sizeof r);GR_A(r,6)=fp;
 P32(mem+fp+24,0x1b000);P32(mem+fp+20,0x1b100);
 memcpy(mem+0x1b000,"OTModl$auxtcp",13);memcpy(mem+0x1b100,"GetOTauxtcpInstallInfo",22);
 aslmgets=128;aslmauxgets=0;
 for(i=0;i<34;i++)aslmsteplog(r,MGET,aslmgetpc);
 assert(aslmauxgets==32&&aslmgets==128);
 ui_aslmclear(&gp);assert(!aslmauxgets&&!aslmgets);
 puts("PASS custom lookup budget survives exhausted stock imports and resets after owned teardown");
 for(i=0;i<6;i++) {
  reset(&gp);uin_aslmreads=101;uin_aslmcode=102;aslmclasses=103;aslmexits=104;
  aslmparentpc=105;aslmfailseen=106;aslmloads=aslmloadexits=aslmgets=aslmauxgets=aslmmpcalls=128;
  if(i)failed=i==1?0x12912:i==2?0x12c78:i==3?0xf572:i==4?0xf646:0xf958;
  aslminstall(0x12c28);
  if(!i) {
   assert(aslmowner==&gp&&!uin_aslmreads&&!uin_aslmcode&&!aslmclasses&&!aslmexits&&!aslmparentpc&&!aslmfailseen);
   assert(!aslmloads&&!aslmloadexits&&!aslmgets&&!aslmauxgets&&!aslmmpcalls);
  } else {
   assert(!aslmowner&&uin_aslmreads==101&&uin_aslmcode==102&&aslmclasses==103&&aslmexits==104&&aslmparentpc==105&&aslmfailseen==106);
   assert(aslmloads==128&&aslmloadexits==128&&aslmgets==128&&aslmauxgets==128&&aslmmpcalls==128);
   assert(G16(mem+0x12912)==0xddae&&G16(mem+0x12c78)==0x302e&&G16(mem+0xf572)==0x7600&&G16(mem+0xf646)==0x3004&&G16(mem+0xf958)==0x286e);
  }
  failed=0;if(aslmowner)ui_aslmclear(&gp);
 }
 puts("PASS fresh owner resets saturated prior budgets only after all five writes; every partial install retains budgets and rolls back");
 for(i=0;i<6;i++) {
  reset(&gp);aslminstall(0x12c28);
  uin_aslmreads=101;uin_aslmcode=102;aslmclasses=103;aslmexits=104;
  aslmparentpc=105;aslmfailseen=106;aslmauxgets=107;
  if(i==1) { ui_aslmclear(&foreign);assert(aslmowner==&gp); }
  if(i==2)failed=0x12912;
  if(i==3)mem[0x1291f]^=1;
  if(i==4)u.u_procp=&foreign;
  if(i==5) {memcpy(mem+0x12912,aslmclass,2);memcpy(mem+0x12c78,aslmexit,2);}
  if(i!=1)ui_aslmclear(&gp);
  if(i==0 || i==5) {
   assert(!uin_aslmreads&&!uin_aslmcode&&!aslmclasses&&!aslmexits&&!aslmparentpc&&!aslmfailseen&&!aslmauxgets);
   assert(!aslmowner&&G16(mem+0x12912)==0xddae&&G16(mem+0x12c78)==0x302e);
  } else {
   assert(uin_aslmreads==101&&uin_aslmcode==102&&aslmclasses==103&&aslmexits==104&&aslmparentpc==105&&aslmfailseen==106&&aslmauxgets==107);
   assert((i==1)==(aslmowner==&gp));
  }
 }
 puts("PASS six session-counter success, retained-owner, write-failure, changed-signature, foreign-space and already-restored cohorts");

 for(prior=0;prior<2;prior++) {
  reset(&gp);aslminstall(0x12c28);assert(G16(mem+0xfaa8)==RESUME);
  memset(r,0,sizeof r);GR_PC(r)=0xfaa8;GR_USP(r)=sp;GR_A(r,6)=fp;
  GR_A(r,2)=0x19000;GR_D(r,0)=0x12345678;GR_SR(r)=0x2008;
  P32(mem+fp+24,0x1a000);P32(mem+fp+20,0x1a100);
  strcpy((char *)mem+0x1a000,"OTClnt$auxCfig");strcpy((char *)mem+0x1a100,"OTSetupConfigurator");
  gp.gp_flags=prior?GPF_STUB:0;
  assert(aslmstep(&gp,r));f=GR_USP(r);assert(G16(mem+f+18)==MGETEXIT);
  assert(G16(mem+f+4)==0x200a&&G16(mem+f+6)==0x4e71);
  GR_D(r,0)=GR_A(r,2);GR_SR(r)=0x2000;memcpy(post,r,sizeof post);
  assert(finish(r,f)==(prior?2:1));
  assert(GR_PC(r)==0xfaaa&&GR_USP(r)==sp&&GR_D(r,0)==0x19000&&GR_SR(r)==0x2000);
  assert(memcmp(r+4,post+4,62)==0&&aslmsetupreturns==1);
  for(i=0;i<20;i++)aslmsteplog(r,MGETEXIT,0xfaa8);
  assert(aslmsetupreturns==16);
  ui_aslmclear(&gp);assert(!aslmsetupreturns&&G16(mem+0xfaa8)==0x200a);
 }
 reset(&gp);mem[0xfaaf]^=1;aslminstall(0x12c28);assert(!aslmowner);
 reset(&gp);aslmsetupreturns=77;failed=0xfaa8;aslminstall(0x12c28);
 assert(!aslmowner&&aslmsetupreturns==77&&G16(mem+0xfaa8)==0x200a&&G16(mem+0xf958)==0x286e);
 failed=0;aslminstall(0x12c28);assert(aslmowner==&gp&&!aslmsetupreturns);
 memset(r,0,sizeof r);GR_A(r,6)=fp;GR_D(r,0)=0;
 P32(mem+fp+24,0x1a000);P32(mem+fp+20,0x1a100);
 strcpy((char *)mem+0x1a000,"OTClnt$foreign");strcpy((char *)mem+0x1a100,"OTSetupConfigurator");
 aslmsteplog(r,MGETEXIT,0xfaa8);assert(!aslmsetupreturns);
 strcpy((char *)mem+0x1a000,"OTClnt$auxCfig");aslmsteplog(r,MGETEXIT,0xfaa8);assert(aslmsetupreturns==1);
 strcpy((char *)mem+0x1a000,"OTModl$auxtcp");strcpy((char *)mem+0x1a100,"GetOTInstallInfo");
 aslmsteplog(r,MGETEXIT,0xfaa8);assert(aslmsetupreturns==2);
 strcpy((char *)mem+0x1a100,"GetOTauxtcpInstallInfo");
 aslmsteplog(r,MGETEXIT,0xfaa8);assert(aslmsetupreturns==3);
 strcpy((char *)mem+0x1a000,"OTModl$auxudp");strcpy((char *)mem+0x1a100,"GetOTauxudpInstallInfo");
 aslmsteplog(r,MGETEXIT,0xfaa8);assert(aslmsetupreturns==4);
 strcpy((char *)mem+0x1a100,"GetOTauxtcpInstallInfo");
 aslmsteplog(r,MGETEXIT,0xfaa8);assert(aslmsetupreturns==4);
 memset(mem+0x1a000,'x',80);aslmsteplog(r,MGETEXIT,0xfaa8);assert(aslmsetupreturns==4);
 ui_aslmclear(&gp);
 puts("PASS returned setup pointer: nested instruction/CCR preservation, own-class bounds, cap, rollback and teardown");
 reset(&gp);aslminstall(0x12c28);
 P32(mem+0x19000,0x4f544447);P32(mem+0x19004,1);P32(mem+0x19008,3);
 aslmowncache(0,0x19000);assert(aslmdiagptr[0]==0x19000);
 aslmowncache(1,0x19000);assert(!aslmdiagptr[1]);
 aslmowncache(2,0x7fffffc0);assert(!aslmdiagptr[2]);
 aslmowncache(2,0x8fffc);assert(!aslmdiagptr[2]);
 aslmowndiags();assert(aslmdiagsamples==1);aslmowndiags();assert(aslmdiagsamples==1);
 P32(mem+0x19010,1);memset(r,0,sizeof r);GR_A(r,6)=fp;
 aslmsteplog(r,MGETEXIT,0xfaa8);assert(aslmdiagsamples==2);
 P32(mem+0x19004,2);P32(mem+0x19010,2);aslmowndiags();assert(aslmdiagsamples==2);
 P32(mem+0x19004,1);
 for(i=0;i<80;i++){P32(mem+0x19010,i+10);aslmowndiags();}
 assert(aslmdiagsamples==64);ui_aslmclear(&gp);
 assert(!aslmdiagptr[0]&&!aslmdiagptr[1]&&!aslmdiagptr[2]&&!aslmdiagsamples);
 reset(&gp);aslmdiagptr[0]=0x19000;aslmdiagsamples=37;failed=0xfaa8;
 aslminstall(0x12c28);assert(!aslmowner&&aslmdiagptr[0]==0x19000&&aslmdiagsamples==37);
 failed=0;aslminstall(0x12c28);assert(aslmowner==&gp&&!aslmdiagptr[0]&&!aslmdiagsamples);
 ui_aslmclear(&gp);
 puts("PASS late own diagnostics: magic/version/type/bounds, later getters, changed-only snapshots, cap, rollback and session reset");
 reset(&gp);aslminstall(0x12c28);
 P32(mem+0x1a000,0x4f544447);P32(mem+0x1a004,1);P32(mem+0x1a008,1);
 P32(mem+0x1a040,0x4f545450);P32(mem+0x1a044,0x0001001c);P32(mem+0x1a048,1);
 aslmowncache(1,0x1a000);aslmowndiags();assert(aslmdiagsamples==1);
 assert(G32(aslmdiaglast[1]+64)==0x4f545450 && G32(aslmdiaglast[1]+72)==1);
 aslmowndiags();assert(aslmdiagsamples==1);
 P32(mem+0x1a048,2);aslmowndiags();assert(aslmdiagsamples==2);
 assert(G32(aslmdiaglast[1]+72)==2);
 P32(mem+0x1a044,0x0002001c);aslmowndiags();assert(aslmdiagsamples==3);
 assert(!G32(aslmdiaglast[1]+64));
 P32(mem+0x1a044,0x0001001d);aslmowndiags();assert(aslmdiagsamples==3);
 P32(mem+0x1a044,0x0001001c);copylimit=0x1a048;aslmowndiags();
 assert(aslmdiagsamples==3 && !G32(aslmdiaglast[1]+64));
 copylimit=0x20000;P32(mem+0x1a040,0);aslmowndiags();assert(aslmdiagsamples==3);
 P32(mem+0x1a040,0x4f545450);aslmowndiags();assert(aslmdiagsamples==4);
 ui_aslmclear(&gp);assert(!aslmdiagsamples&&!aslmdiagptr[1]);
 reset(&gp);aslminstall(0x12c28);aslmdiagptr[1]=0x1a000;
 P32(mem+0x1a000,0x4f544447);P32(mem+0x1a004,1);P32(mem+0x1a008,1);P32(mem+0x1a040,0x4f545450);
 P32(mem+0x1a044,0x0002002c);P32(mem+0x1a070,129);P32(mem+0x1a0ac,1);
 aslmowndiags();assert(G32(aslmdiaglast[1]+112)==129 && G32(aslmdiaglast[1]+172)==1);
 P32(mem+0x1a044,0x0002002d);aslmowndiags();assert(G32(aslmdiaglast[1]+112)==0);
 P32(mem+0x1a044,0x0002002c);copylimit=0x1a070;aslmowndiags();
 assert(G32(aslmdiaglast[1]+112)==0);copylimit=sizeof mem;
 P32(mem+0x1a044,0x00030058);P32(mem+0x1a0b0,1);P32(mem+0x1a150,0x1020304);
 {int before;P32(mem+0x1a0a0,0x7fffffff);before=reads;aslmowndiags();assert(reads==before+3);}
 assert(G32(aslmdiaglast[1]+176)==1 && G32(aslmdiaglast[1]+336)==0x1020304);
 P32(mem+0x1a044,0x00030059);aslmowndiags();assert(!G32(aslmdiaglast[1]+176));
 P32(mem+0x1a044,0x00030058);copylimit=0x1a150;aslmowndiags();assert(!G32(aslmdiaglast[1]+176));copylimit=sizeof mem;

 P32(mem+0x1a044,0x00040078);P32(mem+0x1a160,0x76543210);P32(mem+0x1a1dc,1);
 {int before=reads;aslmowndiags();assert(reads==before+3);}
 assert(G32(aslmdiaglast[1]+352)==0x76543210 && G32(aslmdiaglast[1]+476)==1);
 P32(mem+0x1a044,0x00040079);aslmowndiags();assert(!G32(aslmdiaglast[1]+352));
 P32(mem+0x1a044,0x00040078);copylimit=0x1a1dc;aslmowndiags();assert(!G32(aslmdiaglast[1]+352));copylimit=sizeof mem;
 P32(mem+0x1a044,0x000500a0);P32(mem+0x1a27c,0x87654321);aslmowndiags();assert(G32(aslmdiaglast[1]+636)==0x87654321);
 P32(mem+0x1a044,0x000600b0);P32(mem+0x1a2bc,0x11223344);aslmowndiags();assert(G32(aslmdiaglast[1]+700)==0x11223344);
 P32(mem+0x1a044,0x000700c8);P32(mem+0x1a31c,0x55667788);aslmowndiags();assert(G32(aslmdiaglast[1]+796)==0x55667788);
 P32(mem+0x1a044,0x000a00e0);P32(mem+0x1a37c,0x12345678);aslmowndiags();assert(G32(aslmdiaglast[1]+892)==0x12345678);
 P32(mem+0x1a278,0x1234);copylimit=0x1a27c;aslmowndiags();assert(!G32(aslmdiaglast[1]+636));copylimit=sizeof mem;


 ui_aslmclear(&gp);assert(!aslmdiagptr[1]&&!aslmdiagsamples);
 puts("PASS own TPI extension version/size/kind/bounds/change/refusal/session cases");
 {unsigned char d[176]={0};int before;
 P32(d+24,8);P32(d+164,1);P32(d+160,0x18000);
 P32(mem+0x18000,0x18100);P32(mem+0x1800c,0x18200);P32(mem+0x18010,0x18300);P32(mem+0x18014,0x18400);
 P32(mem+0x18100,0x18500);P32(mem+0x18500,0x4e560000);copylimit=0x20000;
 before=reads;aslmupstream(d,44);assert(reads-before==3);
 before=reads;aslmupstream(d,28);assert(reads==before);
 P32(d+172,1);aslmupstream(d,44);assert(reads==before);P32(d+172,0);
 P32(d+164,0);aslmupstream(d,44);assert(reads==before);P32(d+164,1);
 P32(d+24,7);aslmupstream(d,44);assert(reads==before);P32(d+24,8);
 P32(d+160,0x7fffffe8);aslmupstream(d,44);assert(reads==before);
 P32(d+160,0x18000);copylimit=0x18004;aslmupstream(d,44);assert(reads==before+1);
 copylimit=0x20000;P32(mem+0x18000,0x7ffffffc);before=reads;aslmupstream(d,44);assert(reads==before+1);
 P32(mem+0x18000,0x18100);P32(mem+0x18100,0x7fffffe0);before=reads;aslmupstream(d,44);assert(reads==before+2);
 P32(mem+0x18100,0x18500);copylimit=0x18510;before=reads;aslmupstream(d,44);assert(reads==before+3);
 copylimit=0x20000;
 P32(mem+0x18500,0x4e56ffdc);P32(mem+0x18504,0x48e71c38);P32(mem+0x18508,0x206e0008);P32(mem+0x1850c,0x20280014);
 P32(mem+0x18510,0x2d40ffdc);P32(mem+0x18514,0x246e000c);P32(mem+0x18518,0x206a0014);P32(mem+0x1851c,0x1428000d);
 before=reads;aslmupstream(d,44);assert(reads==before+4);
 mem[0x1851f]^=1;before=reads;aslmupstream(d,44);assert(reads==before+3);mem[0x1851f]^=1;
 P32(mem+0x18014,0x7fffff90);before=reads;aslmupstream(d,44);assert(reads==before+3);
 P32(mem+0x18014,0x18600);copylimit=0x18640;before=reads;aslmupstream(d,44);assert(reads==before+4);
 puts("PASS bounded live upstream queue/callback signature reads and failed/foreign/version guards");}
 assert(flushes>0);puts("PASS production parser guards, nested frame, post-instruction registers/CCR, rollback and owner teardown");return 0;
}
'''
(p/'test.c').write_text(code)
subprocess.run(['cc','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wno-int-to-pointer-cast',str(p/'test.c'),'-o',str(p/'test')],check=True)
subprocess.run([str(p/'test')],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),check=True)
