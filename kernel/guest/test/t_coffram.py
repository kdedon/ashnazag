#!/usr/bin/env python3
"""Check carrier addresses against the current session's shared memory object."""
from pathlib import Path
import os,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'mod/uinter/uinter.c').read_text()
f=s[s.index('int\nui_macrange('):s.index('/*\n * Mac RAM (shm key')]
f+=s[s.index('static int\ncoffbind('):s.index('int\nui_coffworld(')]
w=(Path(__file__).resolve().parents[1]/'mod/uinter/uipatch.c').read_text();f+=w[w.index('long uin_wnestats[4]'):w.index('static int\nfrontcapture(')]
h=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef char *caddr_t;
#define LS_INUSE 2
#define IPC_ALLOC 1
#define APM_COFF 4
#define GPF_AWATCH 8
#define ctob(x) ((unsigned long)(x)<<12)
#define btop(x) ((unsigned long)(x)>>12)
struct proc {long p_pid;struct proc *p_parent;void *p_as;struct guest_proc *gp;};
struct guest_proc {struct proc *gp_proc;int gp_flags,ap_mac;};
#define GUESTP(p) ((p)->gp)
#define AUXP(g) (g)
struct {struct proc *u_procp;} u;
struct {int l_state;struct proc*l_proc;struct guest_proc*l_gp;} ui;
struct {int shmmni;} shminfo;
struct shmid_ds {struct {int mode;long key;} shm_perm;long shm_cpid,shm_segsz;void *shm_amp;} shmem[2];
struct segvn_data {unsigned long anon_index;void *amp;};
struct seg {void *s_ops,*s_data;char *s_base;unsigned long s_size;} segment;
int segvn_ops,joined=1,rangefault,mapfault,fault,checks;
struct seg *as_segat(void *as,char*a){assert(as==u.u_procp->p_as);return mapfault?0:&segment;}
void *ui_task(struct guest_proc*g){return joined?g:0;}
int valid_usr_range(char*a,unsigned long n){return !rangefault;}
struct uicoff {struct uicoff*c_next;long c_pid,c_psn[2],c_a5,c_token;struct proc*c_proc,*c_owner;struct guest_proc*c_gp,*c_ownergp;} record,*ui_coffs=&record;
long ui_coffseq;
struct proc *prfind(long pid){return pid==record.c_pid?record.c_proc:0;}
#define G16(p) ((unsigned long)(unsigned char)(p)[0]<<8|(unsigned char)(p)[1])
#define G32(p) (G16(p)<<16|G16((p)+2))
#define P16(p,v) ((p)[0]=((v)>>8)&255,(p)[1]=(v)&255)
#define P32(p,v) (P16(p,(v)>>16),P16((p)+2,v))
long currenta5;
int copyin(char*a,char*b,int n){if(fault)return 1;if(n==20){memset(b,0,20);P32(b,0x1234);P32(b+4,0xabcd);P32(b+8,1);P32(b+12,0x1fd0000);P16(b+16,0xffff);return 0;}assert(n==4);P32(b,(unsigned long)a==0x904?currenta5:0);return 0;}
#define OK(x) do{assert(x);checks++;}while(0)
'''
t=r'''
int main(void){struct proc owner,child,foreign;struct guest_proc gp,cg;struct segvn_data sv;unsigned char b[20];unsigned long a=30122516;
memset(&owner,0,sizeof owner);memset(&child,0,sizeof child);memset(&gp,0,sizeof gp);memset(&cg,0,sizeof cg);
owner.p_pid=120;owner.p_as=&owner;owner.gp=&gp;gp.gp_proc=&owner;u.u_procp=ui.l_proc=&owner;ui.l_gp=&gp;ui.l_state=2;
child.p_pid=121;child.p_parent=&owner;child.gp=&cg;cg.gp_proc=&child;cg.ap_mac=APM_COFF;
shminfo.shmmni=2;shmem[0].shm_perm.mode=1;shmem[0].shm_perm.key=0x744c4f57;shmem[0].shm_cpid=120;shmem[0].shm_segsz=0x2000000;shmem[0].shm_amp=&sv;
sv.amp=&sv;sv.anon_index=0;segment.s_ops=&segvn_ops;segment.s_data=&sv;segment.s_base=0;segment.s_size=0x2000000;
record.c_pid=121;record.c_proc=&child;record.c_gp=&cg;
#define BAD(expr,undo) do{expr;OK(!ui_macrange(&gp,a,4));undo;}while(0)
OK(ui_macrange(&gp,a,4));OK(ui_macrange(&gp,0x1fffffc,4));OK(!ui_macrange(&gp,0x1fffffe,4));OK(!ui_macrange(&gp,0x2000000,4));OK(!ui_macrange(&gp,a+1,4));OK(!ui_macrange(&gp,a,0));OK(!ui_macrange(&gp,a,0xffffffffUL));
BAD(u.u_procp=&foreign,u.u_procp=&owner);BAD(ui.l_state=0,ui.l_state=2);BAD(joined=0,joined=1);BAD(mapfault=1,mapfault=0);BAD(segment.s_ops=0,segment.s_ops=&segvn_ops);BAD(segment.s_data=0,segment.s_data=&sv);BAD(sv.amp=0,sv.amp=&sv);BAD(shmem[0].shm_cpid=121,shmem[0].shm_cpid=120);BAD(shmem[0].shm_perm.key=0,shmem[0].shm_perm.key=0x744c4f57);BAD(shmem[0].shm_perm.mode=0,shmem[0].shm_perm.mode=1);BAD(rangefault=1,rangefault=0);BAD(sv.anon_index=0x100000,sv.anon_index=0);
segment.s_base=(char *)0x1000;sv.anon_index=1;segment.s_size-=0x1000;OK(ui_macrange(&gp,a,4));segment.s_base=0;sv.anon_index=0;segment.s_size=0x2000000;
P32(b,121);P32(b+4,0);P32(b+8,8194);P32(b+12,a);currenta5=a;
OK(coffbind(&gp,(char *)b)==0&&record.c_a5==a&&record.c_owner==&owner&&G32(b+16)==1);
OK(coffbind(&gp,(char *)b)==0&&G32(b+16)==1);
P32(b+12,a+1);OK(coffbind(&gp,(char *)b)==EINVAL);P32(b+12,a);
currenta5--;OK(coffbind(&gp,(char *)b)==EINVAL);currenta5=a;
fault=1;OK(coffbind(&gp,(char *)b)==EFAULT);fault=0;
child.p_parent=&foreign;OK(coffbind(&gp,(char *)b)==EPERM);child.p_parent=&owner;
P32(b+8,8195);OK(coffbind(&gp,(char *)b)==EBUSY);P32(b+8,8194);
P32(b+12,0x2000000);OK(coffbind(&gp,(char *)b)==EINVAL);
wnecapture(0x1fc0000);OK(uin_wnestats[0]==1&&uin_wnestats[2]==1&&uin_wnelast[2]==1&&uin_wnelast[5]==0x1fc0000+350);
u.u_procp=&foreign;wnecapture(0x1fc0000);OK(uin_wnestats[0]==1);u.u_procp=&owner;
wnecapture(0x2000000);OK(uin_wnestats[0]==1);wnecapture(0xffffff00UL);OK(uin_wnestats[0]==1);
printf("coffram: %d actual mapping/admission checks PASS\n",checks);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(h+f+t)
 subprocess.run(['cc','-std=gnu89','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
