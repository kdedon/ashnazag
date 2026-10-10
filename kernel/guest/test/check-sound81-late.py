import pathlib, subprocess, sys, tempfile
td=tempfile.TemporaryDirectory(prefix='soundnative-');root=pathlib.Path(td.name)
source=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else pathlib.Path(__file__).resolve().parents[3]/'kernel/guest/mod/uinter/uipatch.c'
s=source.read_text();body=s[s.index('long uinter_soundnative'):s.index('/* 1 if _SoundDispatch')]
start=s.index('if (p->p_kind == PSOUND && p->p_sysver == sv');end=s.index('if (p->p_kind == PSPEC',start)
branch=s[start:end]
body+='\nint uinter_trace;long uinter_patched;int earlycalls;\n#define PSOUND 3\n#define GPF_DONE(i) (0x10000UL<<(i))\nint ui_unimpl(void){return G32(mem+TOOLTAB)==G32(mem+TOOLTAB+4*0x9f); }\nint soundearly(void *r,int tw){earlycalls++;return 1;}\nint observe(struct guest_proc *gp,int tw,int sv){struct {int p_kind,p_sysver;} one={3,0x810},*p=&one;int i=5;long pc=0;void *r=0;int once;for(once=0;once<1;once++){'+branch+'}return 0;}\n'
h=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef char *caddr_t;
#define TOOLTAB 0xe00
#define LS_INUSE 1
#define APM_COFF 4
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define SIGSEGV 11
struct proc {void *p_as;};struct guest_proc {struct proc *gp_proc;int gp_vsr;int mac;unsigned long gp_flags;};
struct proc proc;struct guest_proc gp;struct {struct proc *u_procp;}u;
struct {struct guest_proc *l_gp,*l_active;struct proc *l_proc;int l_state;}ui;
#define AUXP(g) (&(struct {int ap_mac;}){(g)->mac})
static unsigned char mem[0x2200000];int foreign,readfail,writefail,restorefail,prot,signalled;
#define G16(p) (((unsigned char *)(p))[0]*256+((unsigned char *)(p))[1])
#define G32(p) ((uint32_t)G16(p)*65536+G16((unsigned char *)(p)+2))
#define P16(p,x) do {((unsigned char *)(p))[0]=(x)>>8;((unsigned char *)(p))[1]=(x);}while(0)
#define P32(p,x) do {uint32_t v_=(x);P16(p,v_>>16);P16((unsigned char *)(p)+2,v_);}while(0)
int ui_macrange(struct guest_proc *g,unsigned long a,int n){return !foreign&&g==&gp&&a>=0x3000&&!(a&1)&&a<sizeof mem&&n<=sizeof mem-a;}
int copyin(caddr_t a,caddr_t b,int n){uintptr_t v=(uintptr_t)a;if(readfail||v>=sizeof mem||n>sizeof mem-v)return 1;memcpy(b,mem+v,n);return 0;}
int copyout(caddr_t a,caddr_t b,int n){uintptr_t v=(uintptr_t)b;if(writefail){writefail=0;memcpy(mem+v,a,2);return 1;}if(restorefail)return 1;memcpy(mem+v,a,n);return 0;}
int as_checkprot(void *a,caddr_t b,int n,int p){return prot;}
void dlm_cacheflush(void){}void psignal(struct proc *p,int s){assert(p==&proc&&s==11);signalled++;}
#define bcmp(a,b,n) memcmp(a,b,n)
'''
t=r'''
#define E 0x2000000
#define G 0x2001000
#define T 0x2002000
#define M 0x2004000
#define V (M-128)
void setup(void){memset(mem,0,sizeof mem);u.u_procp=&proc;gp.gp_proc=&proc;gp.gp_vsr=gp.mac=0;ui.l_gp=ui.l_active=&gp;ui.l_proc=&proc;ui.l_state=1;foreign=readfail=writefail=restorefail=prot=signalled=0;uinter_soundnative=uinter_soundrefused=uinter_soundrollback=0;gp.gp_flags=0;earlycalls=0;P16(mem+0x15a,0x810);P32(mem+TOOLTAB,E);memcpy(mem+E,sndentry,sizeof sndentry);P32(mem+0x2b6,G-0x200);P32(mem+G-0xf0,G);P32(mem+G+0x12,102);P16(mem+G+0x18,1);P32(mem+G+0x26,G+0x100);P32(mem+G+0x2a,T);P32(mem+T+0x28,E+0x100);memcpy(mem+E+0x100,sndadd,sizeof sndadd);P32(mem+T+8,M);memcpy(mem+M,sndmanager,sizeof sndmanager);P16(mem+M-24,0x4efa);P16(mem+M-22,V-(M-22));memcpy(mem+V,sndversion,sizeof sndversion);memcpy(mem+0x33108,sndlegacy,sizeof sndlegacy);memcpy(mem+0x8676,sndcall,sizeof sndcall);}
int main(void){int n=0,i,j;unsigned long spans[]={E,E+0x100,M,V,0x33108};int lengths[]={sizeof sndentry,sizeof sndadd,sizeof sndmanager,sizeof sndversion,sizeof sndlegacy};
setup();assert(soundnative(&gp)==1&&uinter_soundnative==1&&!memcmp(mem+0x8676,sndnop,6));n++;assert(soundnative(&gp)==1&&uinter_soundnative==1);n++;
for(j=0;j<5;j++)for(i=0;i<lengths[j];i++){setup();mem[spans[j]+i]^=1;assert(soundnative(&gp)==0&&!uinter_soundnative&&!memcmp(mem+0x8676,sndcall,8));n++;}
for(i=0;i<8;i++){setup();mem[0x8676+i]^=1;assert(soundnative(&gp)==0&&!uinter_soundnative);n++;}
#define REJECT(x) do {setup();x;assert(soundnative(&gp)==0&&!uinter_soundnative&&!memcmp(mem+0x8676,sndcall,8));n++;}while(0)
REJECT(gp.mac=4);REJECT(gp.gp_vsr=0x100);REJECT(ui.l_gp=0);REJECT(ui.l_active=0);REJECT(ui.l_proc=0);REJECT(ui.l_state=0);REJECT(gp.gp_proc=0);REJECT(foreign=1);REJECT(readfail=1);REJECT(prot=1);REJECT(P16(mem+0x15a,0x761));REJECT(P16(mem+0x15a,0x600));REJECT(P16(mem+0x15a,0x811));REJECT(P32(mem+G+0x12,64));REJECT(P16(mem+G+0x18,0));REJECT(P32(mem+G+0x26,0));REJECT(P32(mem+G+0x2a,0));REJECT(P16(mem+M-24,0x4eba));
setup();writefail=1;assert(soundnative(&gp)==0&&uinter_soundrollback==1&&!uinter_soundnative&&!memcmp(mem+0x8676,sndcall,8));n++;
setup();writefail=restorefail=1;assert(soundnative(&gp)==-1&&signalled==1&&uinter_soundrollback==1&&!uinter_soundnative);n++;
setup();P32(mem+T+8,E+0x200);assert(observe(&gp,0xa800,0x810)==0&&!gp.gp_flags&&!uinter_soundnative);P32(mem+T+8,M);assert(observe(&gp,0xa02e,0x810)==0&&(gp.gp_flags&GPF_DONE(5))&&uinter_soundnative==1);n++;
setup();P32(mem+TOOLTAB+4*0x9f,E);assert(observe(&gp,0xa02e,0x810)==0&&!earlycalls);assert(observe(&gp,0xa800,0x810)==1&&earlycalls==1);n++;
setup();assert(observe(&gp,0xa02e,0x761)==0&&!uinter_soundnative);n++;
printf("PASS %d actual sound adaptation cases: owned32M/signatures/native3/idempotence/rollback/fatalrestore\n",n);}
'''
(root/'native-fixture.c').write_text(h+body+t)
subprocess.run(['gcc','-std=gnu99','-w','-fsanitize=undefined','-fno-sanitize-recover=all',str(root/'native-fixture.c'),'-o',str(root/'native-fixture')],check=True)
subprocess.run([str(root/'native-fixture')],check=True)
harness=pathlib.Path(sys.argv[2]) if len(sys.argv)>2 else source.parents[4]/'tests/src/t_mac.c'
hs=harness.read_text();a=hs.index('t_check("sound_dispatch_initialization"');b=hs.index('/* SimpleText',a);checks=hs[a:b]
norm='''#include <assert.h>
#include <string.h>
#include <stdio.h>
long bits,nativecount;int expected[2],calls;
long getsym(const char *s){return !strcmp(s,"uinter_soundnative")?nativecount:!strcmp(s,"uinter_fmgroup")?0:bits;}
void t_check(const char *s,int yes,const char *fmt,...){assert(calls<2);assert(!!yes==expected[calls++]);}
void run(void){'''+checks+'''}
int main(void){long tests[5][4]={{0x113f,0,1,1},{0x111f,1,1,1},{0x111f,0,0,0},{0x1000,1,1,0},{0x111f,-1,0,0}};int i;for(i=0;i<5;i++){bits=tests[i][0];nativecount=tests[i][1];expected[0]=tests[i][2];expected[1]=tests[i][3];calls=0;run();assert(calls==2);}puts("PASS 5 actual maintained sound path/patch qualification cases");}
'''
(root/'path-fixture.c').write_text(norm)
subprocess.run(['gcc','-std=gnu99','-Wall',str(root/'path-fixture.c'),'-o',str(root/'path-fixture')],check=True)
subprocess.run([str(root/'path-fixture')],check=True)
