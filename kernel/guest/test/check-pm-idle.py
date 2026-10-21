#!/usr/bin/env python3
import pathlib,re,subprocess,sys,tempfile
p=pathlib.Path(sys.argv[1]);s=(p/'uipatch.c').read_text();e=(p/'uievent.c').read_text()
f=s[re.search(r'\nint uin_pmidle(?:=1)?;',s).start()+1:s.index('long uin_wnestats[4]')]
g=e[e.index('long uin_pmidlesleeps,'):e.index('long uin_pollmouse,')]
assert 'uin_pmidlecalls++;ui_pmidlewait(gp);return 0;' in s and 'pmstate==3 || (uin_pmidlestub && pmstate==2)' in s and s.count('pmwatchneeded(gp)')==4
arrays={name:bytes(int(x,0) for x in re.findall(r'0x[0-9a-f]+|\b[0-9]+\b',body)) for name,body in re.findall(r'unsigned char (dispatch|pm28|suffix)\[\]=\{([^}]+)\}',f)}
assert arrays['pm28'][56:64]==bytes.fromhex('4eba725c4eba7254')
assert arrays['suffix'][16:24]==bytes.fromhex('abf7200c6700ff64')
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef char *caddr_t;
#define UI_NTASK 2
#define LM_TICKS 0x16a
#define LS_INUSE 1
#define GPF_AWATCH 8
#define GPF_STUB 1
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
struct uitask {struct guest_proc *t_gp;void *t_proc;};
struct {struct guest_proc *l_gp,*l_active;int l_state,l_tid;struct uitask l_task[UI_NTASK];} ui;
struct proc {int p_cursig,p_pid;unsigned long p_sig,p_hold;} proc;
struct {struct proc *u_procp;char *u_ar0;} u;
unsigned char mem[0x2000000];long ar[8],dr[8],pc,lbolt;int fault,force,in_flags,in_btn,in_btnw,ev_wait,ev_n,ev_q[8],sleeps,interrupts,level,uin_evdiag,step=1;
int copyin(caddr_t a,caddr_t b,unsigned long n){unsigned long x=(uintptr_t)a;assert(x+n<=sizeof mem);if(fault)return 1;memcpy(b,mem+x,n);return 0;}
int copyout(caddr_t a,caddr_t b,unsigned long n){unsigned long x=(uintptr_t)b;assert(x==LM_TICKS&&n==4&&level==0);if(fault)return 1;memcpy(mem+x,a,n);return 0;}
int DS_SPL(int x){int old=level;level=x;return old;}
void DS_SPLX(int x){level=x;}
int ui_forceget(void){return force;}
int sleep(caddr_t p,int flags){assert(p==(caddr_t)ev_q&&flags==(PZERO+1|PCATCH)&&ev_wait==1&&level==DS_HI);sleeps++;lbolt+=step;proc.p_cursig=6;proc.p_sig=0x20;return interrupts;}
'''
pre+='\nint ui_macrange(struct guest_proc *gp,unsigned long a,unsigned long n){return gp->gp_proc==u.u_procp&&a>=0x3000&&!(a&1)&&a<sizeof mem&&n&&n<=sizeof mem-a;}\n'
pre+='\nstatic int pmstate;\nstatic int ev_ticks(struct guest_proc *gp){return 1;}\nstatic void ev_owntick(struct guest_proc *gp){}\n'
f+='\nstatic int pmstubvalid(struct guest_proc *gp){return 0;}\n'
setup='\n'.join('static unsigned char seed_'+k+'[]={'+','.join(hex(x) for x in v)+'};' for k,v in arrays.items())
t=r'''
int main(void){struct guest_proc gp,other;long target=0x3bca5e,site=target+0x8e86,entry=0x3b7c62,sys=0xad4070;int count=0,i,j;unsigned char *where[3];int size[3];
gp.gp_proc=u.u_procp=&proc;gp.gp_vsr=0;gp.gp_flags=0;ui.l_gp=ui.l_active=&gp;ui.l_state=LS_INUSE;ui.l_tid=1;ui.l_task[0].t_gp=&gp;ui.l_task[0].t_proc=u.u_procp;uin_pmidle=1;ar[5]=sys;ar[4]=0;dr[7]=0;pc=site;
P16(mem+0x15a,0x810);P32(mem+0xb7c,sys);P32(mem+0x103c,entry);P32(mem+sys-3308+28*8,target);P16(mem+sys-3308+28*8+4,4);
memcpy(mem+entry,seed_dispatch,sizeof seed_dispatch);memcpy(mem+target,seed_pm28,sizeof seed_pm28);memcpy(mem+site-16,seed_suffix,sizeof seed_suffix);
assert(pmidleowned(&gp,0));count++;
where[0]=mem+entry;where[1]=mem+target;where[2]=mem+site-16;size[0]=sizeof seed_dispatch;size[1]=sizeof seed_pm28;size[2]=sizeof seed_suffix;
for(i=0;i<3;i++)for(j=0;j<size[i];j++){where[i][j]^=1;assert(!pmidleowned(&gp,0));where[i][j]^=1;count++;}
#define BAD(expr,undo) do{expr;assert(!pmidleowned(&gp,0));undo;count++;}while(0)
BAD(uin_pmidle=0,uin_pmidle=1);BAD(ui.l_active=&other,ui.l_active=&gp);BAD(ui.l_gp=&other,ui.l_gp=&gp);BAD(gp.gp_proc=0,gp.gp_proc=u.u_procp);BAD(ui.l_tid=0,ui.l_tid=1);BAD(ui.l_state=0,ui.l_state=LS_INUSE);BAD(gp.gp_vsr=0x100,gp.gp_vsr=0);BAD(ar[4]=1,ar[4]=0);BAD(dr[7]=1,dr[7]=0);BAD(ui.l_task[0].t_gp=0,ui.l_task[0].t_gp=&gp);BAD(ui.l_task[0].t_proc=0,ui.l_task[0].t_proc=u.u_procp);BAD(fault=1,fault=0);BAD(ar[5]++,ar[5]--);BAD(pc++,pc--);BAD(P16(mem+0x15a,0x760),P16(mem+0x15a,0x810));BAD(P16(mem+sys-3308+28*8+4,8),P16(mem+sys-3308+28*8+4,4));BAD(P16(mem+sys-3308+28*8+6,2),P16(mem+sys-3308+28*8+6,0));
ui_pmidlewait(&gp);assert(sleeps==1&&uin_pmidlesleeps==1&&ev_wait==0&&level==0&&G32(mem+LM_TICKS)==(unsigned long)lbolt);count++;
interrupts=1;ui_pmidlewait(&gp);assert(sleeps==2&&uin_pmidleinterrupts==1&&ev_wait==0&&G32(mem+LM_TICKS)==(unsigned long)lbolt);count++;interrupts=0;
#define SKIP(expr,undo) do{expr;ui_pmidlewait(&gp);assert(sleeps==2&&ev_wait==0&&level==0);undo;count++;}while(0)
SKIP(ev_n=1,ev_n=0);SKIP(force=1,force=0);SKIP(in_flags=IN_MOVED,in_flags=0);SKIP(in_flags=IN_KEYS,in_flags=0);SKIP(in_flags=IN_BTN,in_flags=0);SKIP(in_btn=1,in_btn=0);SKIP(ui.l_active=&other,ui.l_active=&gp);SKIP(ui.l_tid=0,ui.l_tid=1);
uin_evdiag=1;proc.p_pid=77;proc.p_cursig=0;proc.p_sig=0;proc.p_hold=0x100;step=0;interrupts=1;
ui_pmidlewait(&gp);assert(uin_pmidlezero==1&&uin_pmidleticks==2&&uin_pmidletracecount==1);assert(uin_pmidletrace[0][0]==uin_pmidletrace[0][1]&&uin_pmidletrace[0][2]==1&&uin_pmidletrace[0][3]==0&&uin_pmidletrace[0][4]==6&&uin_pmidletrace[0][5]==0&&uin_pmidletrace[0][6]==0x20&&uin_pmidletrace[0][7]==0x100&&uin_pmidletrace[0][8]==0x100&&uin_pmidletrace[0][9]==77&&uin_pmidletrace[0][10]==pc&&uin_pmidletrace[0][11]==0);count++;
step=1;for(i=0;i<9;i++){ui_pmidlewait(&gp);count++;}assert(uin_pmidletracecount==8&&uin_pmidletrace[7][1]==uin_pmidletrace[7][0]+1&&uin_pmidleticks==11);
uin_evdiag=0;ui_pmidlewait(&gp);assert(uin_pmidletracecount==8&&uin_pmidleticks==12);count++;
{unsigned long old=G32(mem+LM_TICKS);fault=1;ui_pmidlewait(&gp);assert(uin_pmidleclockfaults==1&&G32(mem+LM_TICKS)==old);fault=0;count++;ui.l_state=0;ui_pmidlewait(&gp);assert(G32(mem+LM_TICKS)==old);ui.l_state=LS_INUSE;count++;gp.gp_proc=0;ui_pmidlewait(&gp);assert(G32(mem+LM_TICKS)==old);gp.gp_proc=u.u_procp;count++;}
printf("pm-idle: %d production guard/wait/signal cases PASS\n",count);return 0;}
'''
for high in (False,True):
 with tempfile.TemporaryDirectory() as d:
  c=pathlib.Path(d)/'test.c';c.write_text(pre+f+g+setup+(t.replace("0x3bca5e","0x13bca5e").replace("0xad4070","0x1fd4070").replace("0x3b7c62","0x13b7c62").replace("0x800000","0x1800000").replace("0x900000","0x1f80000") if high else t));o=pathlib.Path(d)/'test'
  subprocess.run(['cc','-std=gnu89','-fsanitize=undefined','-Werror',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
