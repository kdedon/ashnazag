#!/usr/bin/env python3
"""Exercise the production COFF registry with process and copy-fault fixtures."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'kernel/guest/mod/uinter/uinter.c').read_text()
body=s[s.index('/* Pending applications'):s.index('/*\n * A task that joined leaves')]
a=(root/'kernel/guest/mod/auxcore/auxsys.c').read_text()
pexec=a[a.index('static int\naux_pexec('):a.index('/* the child',a.index('static int\naux_pexec('))]
pexit=a[a.index('static void\naux_pexit('):a.index('/*\n * User ranges',a.index('static void\naux_pexit('))]
h=r'''
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define UI_NTASK 16
#define LS_INUSE 2
#define APM_COFF 4
#define KM_NOSLEEP 0
#define SIGKILL 9
#define GPF_EXEC 8
#define COMPAT_EXEC 0x10
#define COMPAT_DEFAULT 0x403
#define APF_AUX2 1
static int aux_execaux2;
static void (*aux_macdetach)();
void aux_fidexit(){}
void aux_itstop(){}
void aux_alstop(){}
typedef char *caddr_t;
struct cred {int cr_uid;};
struct aux_proc {unsigned ap_mac,ap_compat,ap_flags,ap_sv_onstack,ap_sv_intr;long ap_ss_sp;int ap_ss_onstack;long ap_envp,ap_envc,ap_envlo,ap_envhi;};
struct guest_proc;
struct proc {long p_pid;struct proc *p_parent;struct cred *p_cred;struct guest_proc *gp;};
struct guest_proc {unsigned gp_flags;struct proc *gp_proc;struct aux_proc ap;int joined;};
struct {struct proc *u_procp;struct cred *u_cred;} u;
struct {int l_state;struct proc *l_proc;struct guest_proc *l_gp;} ui;
#define UIACT(g) ((g)==ui.l_gp)
#define AUXP(g) (&(g)->ap)
#define GUESTP(p) ((p)->gp)
static int faultin,faultout,nomem,live,kills,checks;
static struct proc ps[24];static struct guest_proc gs[24];
static struct cred cr[24];
long G32(char *p) {return (long)(unsigned char)p[0]<<24|(long)(unsigned char)p[1]<<16|(long)(unsigned char)p[2]<<8|(unsigned char)p[3];}
int G16(char *p) {return (unsigned char)p[0]<<8|(unsigned char)p[1];}
void P32(char *p,long v) {int i;for(i=3;i>=0;i--){p[i]=v;v>>=8;}}
void P16(char *p,int v) {p[0]=v>>8;p[1]=v;}
static char mem[512];
int copyin(char *a,char *b,int n) {if(faultin)return 1;memcpy(b,mem+(long)a,n);return 0;}
int ui_macrange(struct guest_proc *g,unsigned long a,unsigned long n){return g->gp_proc==u.u_procp&&a<sizeof mem&&n<=sizeof mem-a;}
int valid_usr_range(char *a,unsigned long n){unsigned long x=(unsigned long)a;return x<sizeof mem&&n<=sizeof mem-x;}
int copyout(char *a,char *b,int n) {if(faultout)return 1;memcpy(mem+(long)b,a,n);return 0;}
void *kmem_alloc(int n,int flags){assert(flags==KM_NOSLEEP);if(nomem)return 0;live++;return malloc(n);}
void kmem_free(char *p,int n){live--;free(p);}
struct proc *prfind(long id){int i;for(i=0;i<24;i++)if(ps[i].p_pid==id)return ps+i;return 0;}
void *ui_task(struct guest_proc *g){return g->joined ? g : 0;}
void psignal(struct proc *p,int sig){assert(sig==SIGKILL);kills++;}
void who(int i){u.u_procp=ps+i;u.u_cred=cr+i;}
int request(int i,char *name,int n,int set);
#define OK(x) do{assert(x);checks++;}while(0)
'''
hook=r'''
static void detach(struct guest_proc *g){coffdrop(g,0);}
'''
t=r'''
int request(int i,char *name,int n,int set) {char b[10];P32(b,ps[i].p_pid);P32(b+4,64);P16(b+8,n);if(name)strcpy(mem+64,name);return coffname(u.u_procp->gp,b,set);}
int main(void) {int i;char b[10],old[128];
 for(i=0;i<24;i++){ps[i].p_pid=100+i;ps[i].p_parent=ps;ps[i].p_cred=cr+i;ps[i].gp=gs+i;gs[i].gp_proc=ps+i;cr[i].cr_uid=101;}
 ui.l_state=2;ui.l_proc=ps;ui.l_gp=gs;gs[0].joined=1;who(1);
 OK(request(2,"foreign",7,1)==EPERM && !live);
 OK(request(2,"",0,1)==EPERM && !live);
 nomem=1;OK(request(1,"first",5,1)==ENOMEM && !live && !gs[1].ap.ap_mac);nomem=0;
 faultin=1;OK(request(1,"first",5,1)==EFAULT && !live);faultin=0;
 OK(request(1,"first",5,1)==0 && live==1 && gs[1].ap.ap_mac==APM_COFF);
 faultin=1;OK(request(1,"broken",6,1)==EFAULT);faultin=0;
 who(0);memset(mem+64,0xaa,10);P32(b,101);P32(b+4,64);P16(b+8,0);
 OK(coffname(gs,b,0)==0 && G16(b+8)==6 && !strcmp(mem+64,"first") && (unsigned char)mem[70]==0xaa);
 P16(b+8,2);OK(coffname(gs,b,0)==0 && G16(b+8)==2 && !memcmp(mem+64,"fi",2));
 faultout=1;P16(b+8,0);OK(coffname(gs,b,0)==EFAULT && !G16(b+8));faultout=0;
 ps[1].p_parent=ps+3;P16(b+8,0);OK(coffname(gs,b,0)==EPERM);
 gs[1].joined=1;OK(coffname(gs,b,0)==0 && !strcmp(mem+64,"first"));
 cr[1].cr_uid=102;OK(coffname(gs,b,0)==EPERM);cr[1].cr_uid=101;
 who(2);gs[2].joined=1;OK(coffname(gs+2,b,0)==EPERM);
 OK(request(2,"second",6,1)==0 && live==2);
 who(0);OK(request(1,0,0,0)==0 && !strcmp(mem+64,"first"));
 OK(request(2,0,0,0)==0 && !strcmp(mem+64,"second"));
 who(1);OK(request(1,"",0,1)==0 && live==1 && !gs[1].ap.ap_mac);
 OK(request(1,"first",6,1)==0 && live==2);
 who(0);OK(request(1,0,0,0)==0 && !strcmp(mem+64,"first"));who(1);
 OK(request(1,"",1,1)==0 && live==1 && !gs[1].ap.ap_mac);
 OK(request(1,"again",5,1)==0 && live==2);
 OK(request(1,"too-long",128,1)==EINVAL);
 memset(mem+64,'x',10);mem[68]=0;P32(b,101);P32(b+4,64);P16(b+8,10);OK(coffname(gs+1,b,1)==EINVAL);
 cr[1].cr_uid=102;OK(request(1,"other",5,1)==EPERM);cr[1].cr_uid=101;
 coffdrop(gs+1,0);OK(live==1 && !gs[1].ap.ap_mac);
 who(0);OK(request(1,0,0,0)==ESRCH);
 who(1);OK(request(1,"newexec",7,1)==0 && live==2);
 aux_macdetach=detach;
 gs[1].ap.ap_envp=gs[1].ap.ap_envc=gs[1].ap.ap_envlo=gs[1].ap.ap_envhi=99;
 OK(aux_pexec(gs+1)==0 && live==1 && !gs[1].ap.ap_mac);
 OK(!gs[1].ap.ap_envp&&!gs[1].ap.ap_envc&&!gs[1].ap.ap_envlo&&!gs[1].ap.ap_envhi);
 gs[1].ap.ap_envp=gs[1].ap.ap_envc=gs[1].ap.ap_envlo=gs[1].ap.ap_envhi=99;
 OK(request(1,"exiting",7,1)==0 && live==2);aux_pexit(gs+1);
 OK(live==1 && !gs[1].ap.ap_mac);
 OK(!gs[1].ap.ap_envp&&!gs[1].ap.ap_envc&&!gs[1].ap.ap_envlo&&!gs[1].ap.ap_envhi);
 OK(request(1,"next",4,1)==0 && live==2);
 for(i=3;i<17;i++){who(i);OK(request(i,"pending",7,1)==0);}
 who(17);OK(request(17,"overflow",8,1)==EAGAIN && live==16);
 who(0);ps[2].p_pid=500;P32(b,102);P16(b+8,0);OK(coffname(gs,b,0)==ESRCH);
 coffdrop(gs,1);OK(live==0 && !ui_coffs && !ui_ncoff && kills==14);
 coffdrop(gs,1);OK(live==0 && kills==14);
 printf("coffname: %d production checks PASS\n",checks);return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h+body+hook+pexec+pexit+t)
 subprocess.run(['cc','-std=gnu89','-w','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
