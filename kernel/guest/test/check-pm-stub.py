#!/usr/bin/env python3
import pathlib,re,subprocess,sys,tempfile
p=pathlib.Path(sys.argv[1]);s=(p/'uipatch.c').read_text()
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef char *caddr_t;
#define UI_NTASK 2
#define LS_INUSE 1
#define GPF_AWATCH 8
#define PCATCH 1
#define PZERO 10
#define DS_HI 3
#define IN_MOVED 1
#define IN_KEYS 2
#define IN_BTN 4
#define G16(p) ((unsigned long)((unsigned char*)(p))[0]<<8|((unsigned char*)(p))[1])
#define G32(p) (G16(p)<<16|G16((p)+2))
#define P16(p,v) ((p)[0]=((v)>>8)&255,(p)[1]=(v)&255)
#define P32(p,v) (P16((p),(v)>>16),P16((p)+2,(v)))
#define GR_A(r,n) ar[n]
#define GR_D(r,n) dr[n]
#define GR_PC(r) pc
struct guest_proc {void *gp_proc;int gp_vsr,gp_flags;};
struct uitask {struct guest_proc *t_gp;void *t_proc;int t_force;};
struct {struct guest_proc *l_gp,*l_active;int l_state,l_tid;struct uitask l_task[UI_NTASK];} ui;
struct proc {int p_cursig,p_pid;unsigned long p_sig,p_hold;} proc;
struct {struct proc *u_procp;char *u_ar0;} u;
unsigned char mem[0x2000000];long ar[8],dr[8],pc,lbolt;int fault,force,in_flags,in_btn,in_btnw,ev_wait,ev_n,ev_q[8],sleeps,interrupts,level,uin_evdiag,step=1;
int copyin(caddr_t a,caddr_t b,unsigned long n){unsigned long x=(uintptr_t)a;assert(x+n<=sizeof mem);if(fault)return 1;memcpy(b,mem+x,n);return 0;}
int DS_SPL(int x){int old=level;level=x;return old;}
void DS_SPLX(int x){level=x;}
int ui_forceget(void){return force;}
int sleep(caddr_t p,int flags){assert(p==(caddr_t)ev_q&&flags==(PZERO+1|PCATCH)&&ev_wait==1&&level==DS_HI);sleeps++;lbolt+=step;proc.p_cursig=6;proc.p_sig=0x20;return interrupts;}
'''
pre=pre.replace('dr[8]','dr[9]')
pre=pre.replace('#define GR_A(r,n) ar[n]','#define GR_A(r,n) (((struct word32 *)((r)+36+4*(n)))->v)').replace('#define GR_D(r,n) dr[n]','#define GR_D(r,n) (((struct word32 *)((r)+4+4*(n)))->v)').replace('#define GR_PC(r) pc','#define GR_PC(r) (((struct word32 *)((r)+66))->v)')
pre+='\nstruct word32 {uint32_t v;} __attribute__((packed));\nstruct word16 {uint16_t v;} __attribute__((packed));\n' 
pre+=r'''
#define EINVAL 22
#define GPF_STUB 1
#define SIGKILL 9
#define SIGSEGV 11
#define MPMIDLE 25
#define GR_USP(r) (((struct word32 *)(r))->v)
#define GR_SR(r) (((struct word16 *)((r)+64))->v)
static int pmstate;
#define NPATCH 14
#define GPF_DONE(i) (1UL<<((i)+16))
struct {long ap_mac;} ap;
#define AUXP(gp) (&ap)
int uin_coffdiag;struct guest_proc *coffdiagowner;
long uinter_nalwatch,uin_watchskipped;
struct uitask task;
struct uitask *ui_task(struct guest_proc *gp){return &task;}

int writes,writefail,killed,flushes,allocs,corrupt;
int copyout(caddr_t a,caddr_t b,unsigned long n){unsigned long x=(uintptr_t)b;assert(x+n<=sizeof mem);writes++;if(writefail)return 1;memcpy(mem+x,a,n);if(corrupt==1&&n==60)mem[x]^=1;if(corrupt==2&&n==16)mem[x]=0x99;if(corrupt==3&&n==16)mem[0x800000]^=1;return 0;}
void dlm_cacheflush(void){flushes++;}
void psignal(void*p,int n){assert(n==SIGKILL || n==SIGSEGV);killed++;}
int ui_auxcall(char*r,int sel,long arg,unsigned short*code,int mark,int ext){assert(mark==MPMIDLE&&code[2]==60);allocs++;return 1;}
int waitticks,waitnativeticks;void ui_pmidlewait(struct guest_proc *gp){sleeps++;lbolt+=waitticks;P32(mem+0x16a,G32(mem+0x16a)+waitnativeticks);}
'''
pre+='\nint ui_macrange(struct guest_proc *gp,unsigned long a,unsigned long n){return gp->gp_proc==u.u_procp&&a>=0x3000&&!(a&1)&&a<sizeof mem&&n&&n<=sizeof mem-a;}\n'
f=s[re.search(r'\nint uin_pmidle(?:=1)?;',s).start():s.index('long uin_wnestats[4]')]
g=s[s.index('static void\npmexpected('):s.index('\n#define MCOFFSPACE')]
arrays={name:bytes(int(x,0) for x in re.findall(r'0x[0-9a-f]+|\b[0-9]+\b',body)) for name,body in re.findall(r'unsigned char (dispatch|pm28|suffix)\[\]=\{([^}]+)\}',f)}
def block(at):
 a=s.index(at);j=s.index('{',a);depth=1;k=j+1
 while depth:
  depth+=(s[k]=='{')-(s[k]=='}');k+=1
 return s[a:k]
entry=block('if(pmidleowned(gp,r))')
resume=block('if (G16(b) == RESUME && (i = ui_resume(r, pc)) != 0)')
g+='\nstatic int attempt(struct guest_proc *gp,char *r){'+entry+'return 0;}\n'
g+='\nint resumecode;int ui_resume(char*r,long pc){return resumecode;}void ui_forceend(struct guest_proc*gp){}\n#define RESUME 0xa89f\nstatic int resumehook(struct guest_proc*gp,char*r){unsigned char b[2];int i;long pc=GR_PC(r);P16(b,RESUME);'+resume+'return 0;}\n'
setup='\n'.join('static unsigned char seed_'+k+'[]={'+','.join(hex(x) for x in v)+'};' for k,v in arrays.items())
t=r'''
int main(void){struct guest_proc gp,other;long target=0x3bca5e,site=target+0x8e86,entry=0x3b7c62,sys=0xad4070;int i,n=0,before;unsigned char code[60],redirect[16];char r[128]={0};
gp.gp_proc=u.u_procp=&proc;gp.gp_vsr=0;gp.gp_flags=0;ui.l_gp=ui.l_active=&gp;ui.l_state=LS_INUSE;ui.l_tid=1;ui.l_task[0].t_gp=&gp;ui.l_task[0].t_proc=u.u_procp;uin_pmidle=uin_pmidlestub=1;ar[5]=sys;ar[4]=0;dr[7]=0;pc=site;GR_PC(r)=site;GR_A(r,5)=sys;
P16(mem+0x15a,0x810);P32(mem+0xb7c,sys);P32(mem+0x103c,entry);P32(mem+sys-3308+28*8,target);P16(mem+sys-3308+28*8+4,4);
memcpy(mem+entry,seed_dispatch,sizeof seed_dispatch);memcpy(mem+target,seed_pm28,sizeof seed_pm28);memcpy(mem+site-16,seed_suffix,sizeof seed_suffix);
gp.gp_flags=GPF_STUB;assert(!pmstubstart(&gp,r)&&!allocs);gp.gp_flags=0;n++;
assert(attempt(&gp,r)&&pmstate==1&&allocs==1&&uin_pmidlerequests==1&&(gp.gp_flags&GPF_STUB));n++;
assert(!pmstubstart(&gp,r)&&allocs==1);n++;
before=writes;pmstubfinish(&other,r,0x800000);assert(pmstate==1&&pmowner==&gp&&writes==before);n++;
pmstubfinish(&gp,r,0x800000);resumecode=1;assert(resumehook(&gp,r)&&!(gp.gp_flags&GPF_STUB));n++;gp.gp_flags|=GPF_STUB;resumecode=2;assert(resumehook(&gp,r)&&(gp.gp_flags&GPF_STUB));n++;resumecode=3;assert(resumehook(&gp,r)&&!(gp.gp_flags&GPF_STUB));n++;assert(pmstate==2&&pmstubvalid(&gp)&&pmidleowned(&gp,r)&&uin_pmidleallocations==1&&!uin_pmidlerestores);n++;
{unsigned char before[128];long oldpc;GR_USP(r)=0x900000;P16(mem+0x90000c,0x15);memcpy(mem+0x90000e,r+4,60);GR_PC(r)=pmblock+0x16;GR_D(r,0)=38;memcpy(before,r,128);oldpc=GR_PC(r);assert(!uin_pmidlewatchedcalls);assert(!pmstubpoll(&gp,r)&&sleeps==1&&!memcmp(before,r,128));n++;gp.gp_flags=GPF_AWATCH;assert(!pmstubpoll(&gp,r)&&sleeps==2&&uin_pmidlewatchedcalls==1);gp.gp_flags=0;n++;GR_PC(r)++;assert(pmstubpoll(&gp,r)==EINVAL&&sleeps==2&&uin_pmidlewatchedcalls==1);n++;GR_PC(r)=site;GR_D(r,0)=0;}
{int old=sleeps,j;long oldtick=lbolt;unsigned char before[128];
GR_USP(r)=0x900000;GR_A(r,6)=0x900100;P32(mem+0x900104,0x45678);memcpy(mem+0x90000e,r+4,60);GR_PC(r)=pmblock+0x16;memcpy(before,r,128);
ap.ap_mac=4;task.t_force=4;uin_coffdiag=1;coffdiagowner=&gp;uinter_nalwatch=100;uin_watchskipped=80;
uin_pmphaseon=1;uin_pmphasecount=0;pmphaseseen=0;lbolt=100;waitticks=1;waitnativeticks=1;P32(mem+0x16a,0x1234);proc.p_hold=0x100;
assert(!pmstubpoll(&gp,r)&&uin_pmphasecount==1&&uin_pmphase[0][0]==-1&&uin_pmphase[0][1]==100&&uin_pmphase[0][2]==101);n++;
assert(uin_pmphase[0][3]==site&&uin_pmphase[0][4]==0x900100&&uin_pmphase[0][5]==0x45678&&uin_pmphase[0][6]==0x90004a&&uin_pmphase[0][7]==sys&&!memcmp(before,r,128));n++;
assert(uin_pmwatch[0][0]==4&&uin_pmwatch[0][1]==gp.gp_flags&&uin_pmwatch[0][2]==(((GPF_DONE(NPATCH)-GPF_DONE(0))&~gp.gp_flags)>>16));n++;
assert(uin_pmwatch[0][3]==force&&uin_pmwatch[0][4]==4&&uin_pmwatch[0][5]==1&&uin_pmwatch[0][6]==100&&uin_pmwatch[0][7]==80);n++;
task.t_force=0;uin_coffdiag=0;ap.ap_mac=0;
assert(uin_pmphase[0][12]==0x1234&&uin_pmphase[0][13]==0x1235&&!uin_pmphase[0][14]&&uin_pmphase[0][15]==0x100);n++;
lbolt=103;assert(!pmstubpoll(&gp,r)&&uin_pmphase[1][0]==101&&uin_pmphase[1][1]==103&&uin_pmphase[1][2]==104);n++;
assert(!uin_pmwatch[1][0]&&!uin_pmwatch[1][4]&&!uin_pmwatch[1][5]);n++;
for(j=0;j<10;j++)assert(!pmstubpoll(&gp,r));assert(uin_pmphasecount==8);n++;
uin_pmphaseon=0;assert(!pmstubpoll(&gp,r)&&!pmphaseseen&&uin_pmphasecount==8);n++;
uin_pmphaseon=1;uin_pmphasecount=0;GR_A(r,6)=0xffffffffUL;memcpy(mem+0x90000e,r+4,60);assert(!pmstubpoll(&gp,r)&&uin_pmphase[0][5]==-1);n++;
uin_pmphaseon=0;uin_pmphasecount=0;GR_PC(r)=site;GR_A(r,6)=0;sleeps=old;lbolt=oldtick;waitticks=waitnativeticks=0;proc.p_hold=0;
}
{int old=sleeps;GR_D(r,7)=1;GR_PC(r)=site;assert(!pmidleowned(&gp,r));n++;
assert(pmidleframe(&gp,r,1));n++;GR_USP(r)=0x900000;memcpy(mem+0x90000e,r+4,60);P16(mem+0x90000c,0x15);GR_PC(r)=pmblock+0x16;
assert(!pmstubpoll(&gp,r)&&sleeps==old+1&&uin_pmidlebranches[1]==1);n++;GR_D(r,7)=0;GR_PC(r)=site;}
pmexpected(code,redirect);assert(G32(code+50)==site+8&&G32(code+56)==site&&G32(redirect+2)==0x800000);n++;
for(i=0;i<60;i++){mem[pmblock+i]^=1;assert(!pmstubvalid(&gp));mem[pmblock+i]^=1;n++;}
for(i=0;i<24;i++){mem[site-16+i]^=1;assert(!pmstubvalid(&gp));mem[site-16+i]^=1;n++;}
before=writes;assert(!ui_pmidleclear(&other)&&writes==before&&pmstate==2);n++;
assert(!ui_pmidleclear(&gp)&&pmstate==0&&!pmowner&&!memcmp(mem+site,pmnative,8)&&!memcmp(mem+site-16,pmprefix,16)&&uin_pmidlerestores==1);n++;
assert(pmstubstart(&gp,r));pmstubfinish(&gp,r,0);assert(pmstate==3&&!pmstubstart(&gp,r));n++;
assert(!ui_pmidleclear(&gp)&&pmstate==0);n++;
assert(pmstubstart(&gp,r));pmstubfinish(&gp,r,0x800000);mem[pmblock]^=1;before=writes;assert(!ui_pmidleclear(&gp)&&writes==before+1&&!pmowner);n++;
memcpy(mem+site,pmnative,8);memcpy(mem+site-16,pmprefix,16);assert(pmstubstart(&gp,r));writefail=1;pmstubfinish(&gp,r,0x800000);assert(pmstate==3);writefail=0;assert(!ui_pmidleclear(&gp));n++;
assert(pmstubstart(&gp,r));pmstubfinish(&gp,r,0x800000);writefail=1;assert(ui_pmidleclear(&gp)==4&&pmowner==&gp&&pmstate==2);writefail=0;assert(!ui_pmidleclear(&gp));n++;
memcpy(mem+site,pmnative,8);memcpy(mem+site-16,pmprefix,16);assert(pmstubstart(&gp,r));before=writes;corrupt=1;pmstubfinish(&gp,r,0x800000);corrupt=0;assert(pmstate==3&&writes==before+1&&!memcmp(mem+site,pmnative,8)&&!memcmp(mem+site-16,pmprefix,16));assert(!ui_pmidleclear(&gp));n++;
assert(pmstubstart(&gp,r));before=writes;corrupt=2;pmstubfinish(&gp,r,0x800000);corrupt=0;assert(pmstate==3&&writes==before+2&&mem[site-16]==0x99&&killed);assert(!ui_pmidleclear(&gp));n++;
memcpy(mem+site,pmnative,8);memcpy(mem+site-16,pmprefix,16);assert(pmstubstart(&gp,r));before=writes;corrupt=3;pmstubfinish(&gp,r,0x800000);corrupt=0;assert(pmstate==3&&writes==before+3&&!memcmp(mem+site,pmnative,8)&&!memcmp(mem+site-16,pmprefix,16));assert(!ui_pmidleclear(&gp));n++;
memcpy(mem+site,pmnative,8);memcpy(mem+site-16,pmprefix,16);assert(pmstubstart(&gp,r));pmstubfinish(&gp,r,0x800000);corrupt=2;assert(ui_pmidleclear(&gp)==4&&pmowner==&gp&&mem[site-16]==0x99);corrupt=0;memcpy(mem+site,pmnative,8);memcpy(mem+site-16,pmprefix,16);assert(!ui_pmidleclear(&gp));n++;
printf("pm-stub: %d actual ownership/publication/guard/teardown cases PASS\n",n);return 0;}
'''
for high in (False,True):
 with tempfile.TemporaryDirectory() as d:
  c=pathlib.Path(d)/'test.c';c.write_text((pre.replace("0x800000","0x1800000") if high else pre)+f+g+setup+(t.replace("0x3bca5e","0x13bca5e").replace("0xad4070","0x1fd4070").replace("0x3b7c62","0x13b7c62").replace("0x800000","0x1800000").replace("0x900000","0x1f80000").replace("0x90000c","0x1f8000c").replace("0x90000e","0x1f8000e").replace("0x900100","0x1f80100").replace("0x900104","0x1f80104").replace("0x90004a","0x1f8004a") if high else t));o=pathlib.Path(d)/'test'
  subprocess.run(['cc','-std=gnu89','-fsanitize=undefined','-Werror',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
