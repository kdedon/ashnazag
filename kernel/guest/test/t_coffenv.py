#!/usr/bin/env python3
"""Check the loader's initial environment and session-scoped publication."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'mod/auxexec/auxexec.c').read_text();capture=s[s.index('long aux_envdiag[14];'):s.index('int\naux_coffexec(')]
s=(root/'mod/uinter/uinter.c').read_text();fetch=s[s.index('long uin_envdiag['):s.index('/*\n * A task that joined leaves')]
h=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef char *caddr_t;
struct uarg {int argc,prefixc,envc,stringsize,auxsize;char *stackend,*estkstart;int estksize;char *stacklow;};
struct aux_proc {long ap_envp,ap_envc,ap_envlo,ap_envhi;};
struct guest_proc {void *gp_prof;struct proc *gp_proc;struct aux_proc ap;};
struct proc {struct guest_proc *p_evpdp;long p_pid;};
struct {struct proc *u_procp;} u;
static int aux_profile,foreign;
#define GUESTP(p) ((p)->p_evpdp)
#define AUXP(g) (&(g)->ap)
static struct guest_proc *active;
#define UIACT(g) ((g)==active)
static int joined=1;
void *ui_task(struct guest_proc *g){return joined?g:0;}
static unsigned char mem[0x200000];static int fault,rangefault;static unsigned long membase;
int valid_usr_range(char *a,unsigned long n){unsigned long v=(unsigned long)a;return !rangefault&&v>=membase&&v-membase<sizeof mem&&n<=sizeof mem-(v-membase);}
int copyin(caddr_t src,caddr_t dst,int n){unsigned long a=(unsigned long)src;if(a<membase)return 1;a-=membase;if(fault||a>sizeof mem||n>sizeof mem-a)return 1;memcpy(dst,mem+a,n);return 0;}
#define G16(p) ((unsigned long)(unsigned char)(p)[0]<<8|(unsigned char)(p)[1])
#define G32(p) (G16(p)<<16|G16((p)+2))
#define P16(p,v) ((p)[0]=((v)>>8)&255,(p)[1]=(v)&255)
#define P32(p,v) (P16(p,(v)>>16),P16((p)+2,v))
static int checks;
#define OK(x) do {assert(x);checks++;}while(0)
'''
t=r'''
int main(void){
 struct proc p,other;struct guest_proc g;struct uarg a;unsigned char b[8];unsigned long sp,env;int k;
 memset(&g,0,sizeof g);p.p_pid=12;p.p_evpdp=&g;u.u_procp=&p;g.gp_proc=&p;g.gp_prof=&aux_profile;active=&g;
 a.estkstart=0;a.estksize=0;a.stacklow=0;a.argc=2;a.prefixc=1;a.envc=4;a.stringsize=200;a.auxsize=16;a.stackend=(char *)0xf000;
 sp=0xf000-216-4*(2+1+4+3);env=sp+4*(2+1+2);
 P32(mem+sp,3);P32(mem+env-4,0);P32(mem+env+16,0);
 for(k=0;k<4;k++)P32(mem+env+4*k,env+20+20*k);
 strcpy((char *)mem+env+20,"HOME=/home/user");strcpy((char *)mem+env+40,"TBSYSTEM=custom");
 aux_envsave(&a);OK(g.ap.ap_envp==env&&g.ap.ap_envc==4&&g.ap.ap_envlo==sp&&g.ap.ap_envhi==0xf000);
 OK(coffenv(&g,(char *)b)==0&&G32(b)==env&&G32(b+4)==4);
 OK(!strcmp((char *)mem+G32(mem+env+4),"TBSYSTEM=custom"));
 for(k=0;k<8;k++){
  int oldargc=a.argc,oldprefix=a.prefixc,oldenv=a.envc,oldsize=a.stringsize,oldaux=a.auxsize;char *oldend=a.stackend;
  if(k==0)a.argc=-1;if(k==1)a.envc=16385;if(k==2)a.prefixc=-1;if(k==3)a.stringsize=-1;
  if(k==4)a.auxsize=1048577;if(k==5)a.stackend=(char *)128;
  if(k==6)P32(mem+sp,99);if(k==7)fault=1;
  aux_envsave(&a);OK(!g.ap.ap_envp&&!g.ap.ap_envc&&!g.ap.ap_envlo&&!g.ap.ap_envhi);
  a.argc=oldargc;a.prefixc=oldprefix;a.envc=oldenv;a.stringsize=oldsize;a.auxsize=oldaux;a.stackend=oldend;fault=0;P32(mem+sp,3);
 }
 aux_envsave(&a);active=0;OK(coffenv(&g,(char *)b)==EPERM);active=&g;
 joined=0;OK(coffenv(&g,(char *)b)==EPERM);joined=1;
 u.u_procp=&other;OK(coffenv(&g,(char *)b)==EPERM);u.u_procp=&p;
 for(k=0;k<6;k++){
  aux_envsave(&a);
  if(k==0)g.ap.ap_envp++;if(k==1)g.ap.ap_envhi=g.ap.ap_envp+8;if(k==2)P32(mem+env,0);
  if(k==3)P32(mem+env,0x300000);if(k==4)P32(mem+env+16,1);if(k==5)fault=1;
  OK(coffenv(&g,(char *)b)==(k==3||k==5?EFAULT:EINVAL));fault=0;P32(mem+env,env+20);P32(mem+env+16,0);
 }
 strcpy((char *)mem+0x800,"TBSYSTEM=/Mac/8.1/System Folder");
 P32(mem+env,0x800);OK(coffenv(&g,(char *)b)==0&&G32(b)==env&&uin_envdiag[1]==5);
 P32(mem+env,0x1fffff);mem[0x1fffff]=0;OK(coffenv(&g,(char *)b)==0);
 mem[0x1fffff]=1;OK(coffenv(&g,(char *)b)==EFAULT&&uin_envdiag[1]==6&&uin_envdiag[6]==0&&uin_envdiag[7]==0x1fffff);
 memset(mem+0x100000,'x',1048576);P32(mem+env,0x100000);
 OK(coffenv(&g,(char *)b)==EINVAL&&uin_envdiag[1]==6);
 memset(mem+0x100000,0,1048576);P32(mem+env,env+20);
 a.envc=0;sp=0xf000-216-4*(2+1+0+3);env=sp+20;P32(mem+sp,3);P32(mem+env-4,0);P32(mem+env,0);
 aux_envsave(&a);OK(g.ap.ap_envp==env&&g.ap.ap_envc==0&&coffenv(&g,(char *)b)==0);
 membase=0xc07f0000UL;a.stackend=(char *)0xc0800000UL;
 sp=0xc0800000UL-216-4*(2+1+0+3);env=sp+20;
 P32(mem+sp-membase,3);P32(mem+env-membase-4,0);P32(mem+env-membase,0);
 aux_envsave(&a);OK(aux_envdiag[1]==6&&g.ap.ap_envp==env&&coffenv(&g,(char *)b)==0&&uin_envdiag[1]==5);
 a.envc=1;sp-=4;env-=4;P32(mem+sp-membase,3);P32(mem+env-membase-4,0);P32(mem+env-membase,env+8);P32(mem+env-membase+4,0);
 strcpy((char *)mem+env-membase+8,"TBSYSTEM=actual");aux_envsave(&a);
 OK(aux_envdiag[1]==6&&coffenv(&g,(char *)b)==0&&G32(b)==env&&G32(b+4)==1);
 rangefault=1;aux_envsave(&a);OK(aux_envdiag[1]==4&&!g.ap.ap_envp);rangefault=0;
 fault=1;aux_envsave(&a);OK(aux_envdiag[1]==5&&!g.ap.ap_envp);fault=0;
 printf("coffenv: %d production checks PASS\n",checks);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h+capture+fetch+t)
 subprocess.run(['cc','-std=gnu89','-w','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
