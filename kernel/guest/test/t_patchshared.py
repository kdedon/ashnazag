#!/usr/bin/env python3
"""Check accepted-join cache ownership using production admission and reset code."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'mod/uinter/uinter.c').read_text()
a=(root/'mod/uinter/uipatch.c').read_text()
task=s[s.index('struct uitask *\nui_task('):s.index('/* Accepted joins')]
shared=s[s.index('int\nui_patchshared('):s.index('/* the ui page for p:')]
arm=a[a.index('static int\nhfsreuse('):a.index('/*\n * run the stub')]
h=r'''
#include <assert.h>
#include <stdio.h>
#define UI_NTASK 4
#define LS_INUSE 2
#define APM_TASK 2
#define APM_COFF 4
#define HZ 100
static long lbolt;
#define GR_PC(r) (*(unsigned long *)(r))
#define GR_USP(r) (*(unsigned long *)((r)+4))
#define GR_D(r,n) G32((r)+8+4*(n))
#define GR_A(r,n) G32((r)+40+4*(n))
#define bzero(p,n) __builtin_memset(p,0,n)
#define bcmp(a,b,n) __builtin_memcmp(a,b,n)
#define NPATCH 3
#define GPF_DONE(i) (0x10000UL<<(i))
#define GPF_STUB 0x80000000UL
#define GPF_AWATCH 0x4000
#define PGROUP 4
#define PHFS 8
#define HFSENT 0x580
#define HFSTRAP 0xaffc
typedef char *caddr_t;
static unsigned char mem[0x3000]; static int fault;
#define G16(p) ((unsigned)(unsigned char)(p)[0]<<8|(unsigned char)(p)[1])
#define G32(p) ((unsigned long)G16(p)<<16|G16((p)+2))
#define P16(p,v) ((p)[0]=((v)>>8)&255,(p)[1]=(v)&255)
#define P32(p,v) (P16(p,(v)>>16),P16((p)+2,v))
int copyin(caddr_t src,caddr_t dst,int n) {unsigned long p=(unsigned long)src;if(fault||p+n>sizeof mem)return 1;__builtin_memcpy(dst,mem+p,n);return 0;}
struct proc {int id;};
struct guest_proc {struct proc *gp_proc; unsigned long gp_flags; unsigned ap_mac;};
struct uitask {struct proc *t_proc;struct guest_proc *t_gp;};
struct {int l_state;struct proc *l_proc;struct guest_proc *l_gp;struct uitask l_task[UI_NTASK];} ui;
struct {struct proc *u_procp;} u;
#define AUXP(g) (g)
static long hfst,trashdir,spectab; static int trashbusy;
static int uinter_fmgroup=1,uinter_patches=1,uinter_trace;
static struct {int p_kind;} uipatches[NPATCH]={{0},{PGROUP},{PHFS}};
'''
t=r'''
int main(void) {
 struct proc owner={1},child={2},other={3};
 struct guest_proc g={&owner,0,APM_TASK},c={&child,0,APM_TASK},x={&other,0,APM_TASK};
 int k,checks=0;
 ui.l_state=LS_INUSE;ui.l_proc=&owner;ui.l_gp=&g;
 ui.l_task[0].t_proc=&owner;ui.l_task[0].t_gp=&g;
 ui.l_task[1].t_proc=&child;ui.l_task[1].t_gp=&c;u.u_procp=&child;
 assert(ui_patchshared(&c));checks++;
 hfst=0x1234;P32(mem+HFSENT,hfst);P16(mem+hfst,HFSTRAP);P16(mem+hfst+2,0x4e75);P16(mem+hfst+4,0x4ef9);P32(mem+hfst+6,0x2222);assert(hfsreuse());checks++;trashbusy=1;trashdir=77;spectab=88;
 c.gp_flags=(GPF_DONE(NPATCH)-GPF_DONE(0))|GPF_STUB|1;
 ui_patcharm(&c);
 assert(hfst==0x1234&&trashbusy==1&&trashdir==77&&spectab==88);checks++;
 assert(c.gp_flags==(GPF_AWATCH|GPF_DONE(2)|1));checks++;
 c.ap_mac|=APM_COFF;uin_coffdiag=1;lbolt=456;
 uin_coffcalls[0]=7;uin_coffcalls[1]=9;uin_coffclosed=4;uin_coffseen=65537;
 for(k=0;k<8;k++)uin_cofflast[k]=77;
 ui_patcharm(&c);
 assert(coffdiagowner==&c && coffdiaguntil==1956 && !coffdiagpolls);checks++;
 assert(!uin_coffcalls[0] && !uin_coffcalls[1] && !uin_coffclosed && !uin_coffseen);checks++;
 for(k=0;k<8;k++) {assert(!uin_cofflast[k]);checks++;}
 assert(hfst==0x1234 && trashbusy==1 && trashdir==77 && spectab==88);checks++;
 c.ap_mac=APM_TASK;uin_coffdiag=0;coffdiagowner=0;lbolt=0;
 for(k=0;k<6;k++) {
  P32(mem+HFSENT,hfst);P16(mem+hfst,HFSTRAP);P16(mem+hfst+2,0x4e75);P16(mem+hfst+4,0x4ef9);P32(mem+hfst+6,0x2222);fault=0;
  if(k==0)P32(mem+HFSENT,0x2000);
  if(k==1)P16(mem+hfst,0xaffd);
  if(k==2)P16(mem+hfst+2,0x4e71);
  if(k==3)P16(mem+hfst+4,0x4efa);
  if(k==4)P32(mem+hfst+6,0x2223);
  if(k==5)fault=1;
  assert(!hfsreuse());checks++;
  ui_patcharm(&c);assert(c.gp_flags&GPF_DONE(2));assert(hfst==0x1234);checks++;
 }
 fault=0;
 for(k=0;k<9;k++) {
  ui.l_state=LS_INUSE;ui.l_proc=&owner;ui.l_gp=&g;g.gp_proc=&owner;
  ui.l_task[0].t_proc=&owner;ui.l_task[0].t_gp=&g;
  ui.l_task[1].t_proc=&child;ui.l_task[1].t_gp=&c;c.ap_mac=APM_TASK;u.u_procp=&child;
  if(k==0)ui.l_state=0;
  if(k==1)ui.l_proc=0;
  if(k==2)ui.l_gp=0;
  if(k==3)ui.l_task[0].t_proc=&other;
  if(k==4)ui.l_task[0].t_gp=&x;
  if(k==5)g.gp_proc=&other;
  if(k==6)ui.l_task[1].t_proc=&other;
  if(k==7)u.u_procp=&other;
  if(k==8)c.ap_mac=0;
  assert(!ui_patchshared(&c));checks++;
  hfst=trashdir=spectab=99;trashbusy=1;ui_patcharm(&c);
  assert(!hfst&&!trashdir&&!spectab&&!trashbusy);checks++;
 }
 ui.l_state=LS_INUSE;ui.l_proc=&owner;ui.l_gp=&g;g.gp_proc=&owner;
 ui.l_task[0].t_proc=&owner;ui.l_task[0].t_gp=&g;u.u_procp=&owner;
 assert(!ui_patchshared(&g)&&!ui_patchshared(&x)&&!ui_patchshared(0));checks++;
 hfst=trashdir=spectab=99;trashbusy=1;uinter_fmgroup=0;
 ui_patcharm(&g);assert(!hfst&&!trashdir&&!spectab&&!trashbusy);checks++;
 assert(g.gp_flags==(GPF_AWATCH|GPF_DONE(1)));checks++;
 printf("patchshared: %d PASS\n",checks);
}
'''
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/'test.c';out=Path(d)/'test';c.write_text(h+task+shared+arm+t)
 subprocess.run([os.getenv('CC','cc'),'-std=gnu89','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"))
