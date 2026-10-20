#!/usr/bin/env python3
import pathlib,subprocess,sys,tempfile
p=pathlib.Path(sys.argv[1])
def function(s,name):
 a=s.index('\n'+name+'(');a=s.rfind('\n',0,a);a=s.rfind('\n',0,a);b=s.index('{',a);n=1;j=b+1
 while n:n+=(s[j]=='{')-(s[j]=='}');j+=1
 return s[a:j]
a=function((p/'uipatch.c').read_text(),'ui_watchneeded');b=function((p/'uievent.c').read_text(),'ui_forcewatch')
pre=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef char *caddr_t;
#define GPF_STUB 1
#define GPF_AWATCH 2
#define GPF_ALINE 4
#define GPF_DONE(i) (0x20000UL<<(i))
#define DS_HI 1
#define LS_INUSE 1
#define UI_NTASK 2
#define NPATCH 14
#define G16(b) ((unsigned char)b[0]*256+(unsigned char)b[1])
struct proc {int pid;} mainproc,foreignproc;
struct guest_proc {struct proc *gp_proc;unsigned long gp_flags;};
struct uitask {struct guest_proc *t_gp;struct proc*t_proc;int t_force;};
struct {struct uitask l_task[2];struct guest_proc*l_gp,*l_active;}ui;
struct {struct proc*u_procp;}u;
struct {int p_sysver;}uipatches[NPATCH];
int pending,forcepending,forcewatching,uin_forcescan,uin_forceoff,uin_forcearm;
int uin_coffdiag,uin_pmidle,uin_pmidlestub,pmstate,uinter_patches=1,fault,sv=0x810,coff;
unsigned long coffdiagpolls;long lbolt,coffdiaguntil;struct guest_proc*coffdiagowner;
struct uitask*ui_task(struct guest_proc*gp){int i;for(i=0;i<2;i++)if(ui.l_task[i].t_gp==gp)return ui.l_task+i;return 0;}
int copyin(caddr_t a,caddr_t b,int n){assert((long)a==0x15a&&n==2);if(fault)return 1;b[0]=sv>>8;b[1]=sv;return 0;}
int ui_forcepollowned(struct guest_proc*gp){struct uitask*t=ui_task(gp);return t&&(t->t_force&4);}
int ui_forceget(void){return pending;}
int spaceowned,spacecalls;
int spacewatch(struct guest_proc*gp){assert(gp->gp_proc==u.u_procp);spacecalls++;return spaceowned;}
int ui_coffworld(struct guest_proc*gp,char*b){return coff;}
int DS_SPL(int n){return 0;}void DS_SPLX(int n){}
'''
main=r'''
int main(void){struct guest_proc gp={&mainproc,GPF_AWATCH|GPF_ALINE},other={&foreignproc,GPF_AWATCH};int i,n=0;
ui.l_task[0].t_gp=ui.l_gp=ui.l_active=&gp;ui.l_task[0].t_proc=u.u_procp=&mainproc;
for(i=0;i<NPATCH;i++)uipatches[i].p_sysver=0x810;
#define RELEASE() do{gp.gp_flags|=GPF_AWATCH;ui.l_task[0].t_force=1;forcewatching=1;ui_forcewatch();assert(!ui.l_task[0].t_force);assert(uin_forceoff>0);}while(0)
for(i=0;i<NPATCH;i++){gp.gp_flags=GPF_ALINE;for(int j=0;j<NPATCH;j++)if(i!=j)gp.gp_flags|=GPF_DONE(j);assert(ui_watchneeded(&gp));RELEASE();assert(gp.gp_flags&GPF_AWATCH);n++;}
gp.gp_flags=GPF_ALINE;for(i=0;i<NPATCH;i++)gp.gp_flags|=GPF_DONE(i);
assert(!ui_watchneeded(&gp));RELEASE();assert(!(gp.gp_flags&GPF_AWATCH));n++;
fault=1;RELEASE();assert(gp.gp_flags&GPF_AWATCH);fault=0;n++;
sv=0x710;RELEASE();assert(gp.gp_flags&GPF_AWATCH);sv=0x810;n++;
uinter_patches=0;spaceowned=1;RELEASE();assert(gp.gp_flags&GPF_AWATCH);n++;
spaceowned=0;RELEASE();assert(!(gp.gp_flags&GPF_AWATCH));n++;
uinter_patches=0;coff=1;RELEASE();assert(gp.gp_flags&GPF_AWATCH);coff=0;n++;
uin_coffdiag=1;coffdiagowner=&gp;coffdiaguntil=1;RELEASE();assert(gp.gp_flags&GPF_AWATCH);coffdiaguntil=0;assert(!ui_watchneeded(&gp));uin_coffdiag=0;n++;
uin_pmidle=1;RELEASE();assert(gp.gp_flags&GPF_AWATCH);uin_pmidlestub=1;pmstate=2;assert(!ui_watchneeded(&gp));pmstate=3;assert(!ui_watchneeded(&gp));uin_pmidlestub=0;assert(!ui_watchneeded(&gp));uin_pmidle=0;n++;
gp.gp_flags|=GPF_STUB;ui.l_task[0].t_force=1;forcewatching=1;ui_forcewatch();assert(ui.l_task[0].t_force==1&&(gp.gp_flags&GPF_AWATCH));gp.gp_flags&=~GPF_STUB;n++;
ui.l_task[0].t_force=5;forcewatching=1;ui_forcewatch();assert(ui.l_task[0].t_force==5&&ui_watchneeded(&gp));ui.l_task[0].t_force=0;n++;
ui.l_task[1].t_gp=&other;ui.l_task[1].t_proc=&foreignproc;ui.l_task[1].t_force=1;forcewatching=1;ui_forcewatch();assert(!ui.l_task[1].t_force&&(other.gp_flags&GPF_AWATCH));n++;
ui.l_task[1].t_proc=&mainproc;assert(!ui_watchneeded(&other));n++;
pending=forcepending=1;gp.gp_flags&=~GPF_AWATCH;ui_forcewatch();assert((gp.gp_flags&GPF_AWATCH)&&(ui.l_task[0].t_force&1));n++;
printf("PASS %d actual watch-ownership cases\n",n);return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
 f=pathlib.Path(tmp)/'test.c';f.write_text(pre+a+b+main)
 subprocess.run(['cc','-std=c99','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(f),'-o',tmp+'/test'],check=True)
 subprocess.run([tmp+'/test'],check=True)
