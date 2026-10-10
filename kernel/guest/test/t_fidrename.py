#!/usr/bin/env python3
"""Execute the production rename adapter against a mounted vnode graph."""
from pathlib import Path
import subprocess, tempfile, os
src=(Path(__file__).resolve().parents[1]/'mod/auxcore/auxfid.c').read_text()
prod=src[src.index('#define FID_PATH'):src.index('/* fidop(op, hdr)')]+src[src.index('void\naux_fidfree()'):]
h=r'''#include <assert.h>
#include <setjmp.h>
typedef struct {jmp_buf v;} label_t;
#undef setjmp
#define setjmp(p) _setjmp((p)->v)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef char *caddr_t; typedef unsigned int u_int; typedef int pid_t;
typedef struct { long r_val1; } rval_t;
struct aux_proc {int dummy;};
struct cred {int ref,ngroups,uid;};typedef struct cred cred_t;static cred_t fid_root={1,0,0}; struct proc {pid_t p_pid;};
struct vfs {struct vnode *vfs_vnodecovered; int id;};
struct vnode {int v_flag,ref,ino; struct vfs *v_vfsp; struct vnode *parent; char *name;};
struct vattr {int va_mask;long va_nodeid,va_fsid;};
struct iovec {caddr_t iov_base; int iov_len;};
struct uio {struct iovec *uio_iov;int uio_iovcnt;off_t uio_offset;int uio_segflg,uio_resid;long uio_limit;};
struct dirent {uint32_t d_ino; int32_t d_off;unsigned short d_reclen;char d_name[1];};
struct fidmsg {struct fidmsg *m_next;long m_id,m_type,m_len;};
struct fidq {struct fidmsg *q_head;int q_count;};
static struct fidq fid_cq,fid_rq;
static struct proc daemon={8},*fid_server=&daemon; static pid_t fid_pid=8;
static unsigned long fid_epoch;static int fid_reserved;
static struct vfs fs={0,1},fs2={0,2};
static struct vnode root={1,1,1,&fs,0,""},home={0,1,2,&fs,&root,"home"},
 mp={0,1,3,&fs,&root,"mnt"},mroot={1,1,4,&fs2,0,""},
 sub={0,1,5,&fs2,&mroot,"sub"};
static struct vnode *rootdir=&root,*cwd=&home,*fid_rootdir=&root;
static struct {struct vnode *u_rdir;struct cred *u_cred;struct proc *u_procp;label_t u_qsav;int u_error,u_syscall;} u;
static struct vnode *nodes[]={&root,&home,&mp,&mroot,&sub};
static int calls,renerr,allocfail,allocs,freed,resetcall,malformed,scanloop,wakes,renamefs=1,wronginode=0,interruptcall=0,mutatecall=0,interruptprep=0,interleavecall=0,refused=0,pending_signal=0;static long *mockargs;
static char renamed[1024];
#define FQ_MAX 20
#define VROOT 1
#define UIO_SYSSPACE 1
#define NO_FOLLOW 0
#define FOLLOW 1
#define NULLVPP 0
#define KM_NOSLEEP 0
#define AT_NODEID 1
#define AT_FSID 2
#define VN_CMP(a,b) ((a)==(b))
#define VN_HOLD(v) ((v)->ref++)
#define VN_RELE(v) ((v)->ref--)
#define VOP_RWLOCK(v) ((void)0)
#define VOP_RWUNLOCK(v) ((void)0)
#define bzero(p,n) memset(p,0,n)
#define bcopy(a,b,n) memmove(b,a,n)
#define P32(p,v) do { unsigned char *q=(void*)(p);uint32_t x=(v);q[0]=x>>24;q[1]=x>>16;q[2]=x>>8;q[3]=x; } while(0)
static struct proc *prfind(pid_t p) {return p==8?&daemon:0;}
static void *kmem_alloc(int n,int flags){if(allocfail && ++allocs==allocfail)return 0;return malloc(n);}
static void kmem_free(void *p,int n){freed++;free(p);}
static void fq_free(struct fidmsg *m){kmem_free(m,sizeof *m+m->m_len);}
static void fq_clear(struct fidq *q){struct fidmsg *m;while((m=q->q_head)){q->q_head=m->m_next;fq_free(m);}q->q_count=0;}
static int copyinstr(caddr_t p,char *out,int cap,u_int *n){if(!p)return EFAULT;int z=strlen(p)+1;if(z>cap)return ENAMETOOLONG;memcpy(out,p,z);*n=z;return 0;}
static void wakeup(caddr_t p){assert(fid_reserved==1);assert(fid_cq.q_head&&fid_cq.q_count<=FQ_MAX);wakes++;if(pending_signal)u.u_error=EINTR;}
static struct vnode *finddir(char *p){
 if(!strcmp(p,"."))return cwd;
 if(!strcmp(p,"/"))return &root;
 if(!strcmp(p,"/home")||!strcmp(p,"/link"))return &home;
 if(!strcmp(p,"/mnt"))return &mroot;
 if(!strcmp(p,"/mnt/sub"))return &sub;
 return 0;
}
static int lookupname(char *p,int space,int follow,void *unused,struct vnode **v){
 static struct vnode file={0,1,77,&fs,&home,"file"};
 *v=finddir(p);if(!*v){
 if(!strcmp(p,"file"))p=cwd==&home?"/home/file":"/mnt/sub/file";
 if(!strncmp(p,"/link/",6))p="/home/file";
 if(!strcmp(p,"new"))p=renamed;
 if(strcmp(p,"/home/file")&&strcmp(p,"/mnt/sub/file")&&strcmp(p,renamed))return ENOENT;
 file.v_vfsp= !strncmp(p,"/mnt/",5)?&fs2:&fs;
 if(!strcmp(p,renamed))file.v_vfsp=renamefs==2?&fs2:&fs;
 if(!strcmp(p,renamed))file.ino=wronginode?88:77;else file.ino=77;
 *v=&file;
 } VN_HOLD(*v);return 0;
}
static int vlookup(struct vnode *v,char *p,struct vnode **o,void *x,int f,struct vnode *r,struct cred *cr){
 int i;if(!strcmp(p,"..")){*o=v->parent?v->parent:v;VN_HOLD(*o);if(interruptprep){u.u_error=EINTR;longjmp(u.u_qsav.v,1);}return 0;}
 for(i=0;i<5;i++)if(nodes[i]->parent==v&&!strcmp(nodes[i]->name,p)){*o=nodes[i];VN_HOLD(*o);return 0;}
 return ENOENT;
}
#define VOP_LOOKUP vlookup
static int vget(struct vnode *v,struct vattr *a,int f,struct cred *c){a->va_nodeid=v->ino;a->va_fsid=v->v_vfsp->id;if(mutatecall&&v->ino==77)mockargs[0]=(long)"changed";return 0;}
#define VOP_GETATTR vget
static int vread(struct vnode *v,struct uio *io,struct cred *cr,int *eof){
 int i,off=0;struct dirent *d;
 for(i=0;i<5;i++)if(nodes[i]->parent==v){d=(void*)(io->uio_iov->iov_base+off);d->d_ino=nodes[i]->ino;strcpy(d->d_name,nodes[i]->name);d->d_reclen=(11+strlen(d->d_name)+3)&~3;off+=d->d_reclen;}
 if(malformed&&off)((struct dirent*)io->uio_iov->iov_base)->d_reclen=0;
 io->uio_resid-=off;io->uio_offset+=scanloop?0:off;*eof=!scanloop;return 0;
}
#define VOP_READDIR vread
int aux_amix(int num,long *a,rval_t *r){assert(num==134);calls++;return renerr;}
static int vn_rename(char *old,char *new,int seg){
 assert(seg==UIO_SYSSPACE && u.u_syscall==134);assert(fid_reserved==1 || fid_rootdir==&home);assert(fid_cq.q_count+fid_reserved<=FQ_MAX);
 assert(!strcmp(old,"file")||old[0]=='/');
 if(interleavecall){if(fid_cq.q_count+fid_reserved>=FQ_MAX)refused++;else fid_cq.q_count++;}
 calls++;if(interruptcall){u.u_error=EINTR;longjmp(u.u_qsav.v,1);}if(resetcall)fid_epoch++;
 if(renerr)return renerr;
 if(!strncmp(new,"/mnt/",5))renamefs=2;else if(new[0]=='/')renamefs=1;else renamefs=cwd==&sub?2:1;
 if(new[0]=='/')strcpy(renamed,new);else {strcpy(renamed,cwd==&home?"/home/":"/mnt/sub/");strcat(renamed,new);}
 if(!strncmp(renamed,"/link/",6)){char tmp[1024];strcpy(tmp,renamed+6);strcpy(renamed,"/home/");strcat(renamed,tmp);}return 0;}

'''
t=r'''static void reset(void){struct fidmsg *m;while((m=fid_cq.q_head)){fid_cq.q_head=m->m_next;fq_free(m);}fid_cq.q_count=0;fid_rq.q_count=0;calls=renerr=allocfail=allocs=resetcall=malformed=scanloop=wakes=0;renamed[0]=0;renamefs=1;wronginode=0;interruptcall=0;mutatecall=0;interruptprep=interleavecall=refused=pending_signal=0;u.u_procp=0;fid_server=&daemon;fid_pid=8;fid_rootdir=&root;u.u_rdir=0;cwd=&home;}
static int run(char *old,char *new){long a[2]={(long)old,(long)new};rval_t rv={0};mockargs=a;return aux_rename(0,a,&rv,0);}
static void check(char *old,char *new){struct fidmsg *m=fid_cq.q_head;unsigned char *b=(void*)(m+1);assert(m&&m->m_type==1&&m->m_id==-1&&fid_cq.q_count==1&&wakes==1);assert(b[0]==0&&b[1]==0&&b[2]==0&&b[3]==77);assert(!strcmp((char*)b+4,old));assert(!strcmp((char*)b+5+strlen(old),new));assert(!b[m->m_len-1]);assert(!fid_clientbusy());assert(!fid_rq.q_count);}
int main(void){int n=0;fs2.vfs_vnodecovered=&mp;
 reset();assert(!run("/home/file","/home/new"));check("/home/file","/home/new");n++;
 reset();assert(!run("file","new"));check("/home/file","/home/new");n++;
 reset();cwd=&sub;assert(!run("file","new"));check("/mnt/sub/file","/mnt/sub/new");n++;
 reset();assert(!run("/link/file","/link/new"));check("/home/file","/home/new");n++;
 reset();renerr=EPERM;assert(run("file","new")==EPERM&&calls==1&&!fid_cq.q_count&&!fid_reserved);n++;
 reset();renerr=EXDEV;assert(run("file","/mnt/sub/new")==EXDEV&&calls==1&&!fid_cq.q_count);n++;
 reset();fid_cq.q_count=20;assert(run("file","new")==ENOMEM&&!calls);fid_cq.q_count=0;n++;
 reset();allocfail=1;assert(run("file","new")==ENOMEM&&!calls);n++;
 reset();allocfail=2;assert(run("file","new")==ENOMEM&&!calls);n++;
 reset();assert(run(0,"new")==EFAULT&&!calls);n++;
 reset();malformed=1;assert(run("file","new")==EIO&&!calls);n++;
 reset();fid_server=0;assert(!run("file","new")&&calls==1&&!fid_cq.q_count);n++;
 reset();resetcall=1;assert(!run("file","new")&&!fid_cq.q_count&&!fid_reserved);n++;
 reset();u.u_rdir=&home;assert(!run("file","new"));check("/home/file","/home/new");n++;
 reset();interruptcall=1;assert(run("file","new")==EINTR&&calls==1&&!fid_cq.q_count&&!fid_reserved);n++;
 reset();struct fidmsg *c=malloc(sizeof *c);memset(c,0,sizeof *c);c->m_id=10;fid_cq.q_head=c;fid_cq.q_count=1;assert(!run("file","new")&&fid_cq.q_head==c&&c->m_next&&fid_clientbusy());n++;
 reset();u.u_procp=&daemon;assert(!run("file","new")&&!fid_cq.q_count);n++;
 reset();fid_pid=99;assert(!run("file","new")&&!fid_cq.q_count);n++;
  reset();scanloop=1;assert(run("/home/file","/home/new")==0);n++;
 reset();struct vnode *prev=home.parent;home.parent=&home;assert(run("file","new")==ELOOP&&!calls);home.parent=prev;n++;
 reset();char longpath[1100];memset(longpath,'x',1099);longpath[1099]=0;assert(run(longpath,"new")==ENAMETOOLONG&&!calls);n++;
 reset();fid_reserved=1;fid_cq.q_count=19;assert(run("file","new")==ENOMEM&&!calls);fid_reserved=0;fid_cq.q_count=0;n++;
 reset();mutatecall=1;assert(!run("file","new"));check("/home/file","/home/new");n++;
 reset();assert(!run("file","new"));fid_dropsilent();assert(!fid_cq.q_head&&!fid_cq.q_count);n++;
 reset();struct fidmsg *keep=malloc(sizeof *keep);memset(keep,0,sizeof *keep);keep->m_id=9;fid_cq.q_head=keep;fid_cq.q_count=1;assert(!run("file","new"));fid_dropsilent();assert(fid_cq.q_head==keep&&!keep->m_next&&fid_cq.q_count==1);n++;
 reset();interruptprep=1;assert(run("file","new")==EINTR&&!fid_cq.q_count&&!fid_reserved&&!calls);n++;
 reset();fid_cq.q_count=19;interleavecall=1;assert(!run("file","new")&&refused==1&&fid_cq.q_count==20&&!fid_reserved);n++;
 reset();pending_signal=1;assert(!run("file","new")&&u.u_error==EINTR&&fid_cq.q_head&&!fid_reserved);n++;
 reset();fid_rootdir=&home;assert(!run("file","new"));check("/file","/new");n++;
 reset();fid_rootdir=&home;assert(!run("/mnt/sub/file","/mnt/sub/new")&&!fid_cq.q_count);n++;
 reset();assert(root.ref==1&&home.ref==1&&mp.ref==1&&mroot.ref==1&&sub.ref==1);n++;
 fid_rootdir=0;u.u_procp=&daemon;fid_register();assert(fid_rootdir==&root&&root.ref==2);n++;
 fid_register();assert(root.ref==2);n++;
 u.u_rdir=&home;fid_register();assert(fid_rootdir==&home&&root.ref==1&&home.ref==2);n++;
 u.u_rdir=0;fid_register();assert(fid_rootdir==&root&&root.ref==2&&home.ref==1);n++;
 aux_fidfree();assert(!fid_rootdir&&!fid_server&&!fid_cq.q_count&&!fid_rq.q_count&&root.ref==1);n++;
 struct proc other={9};u.u_procp=&daemon;fid_register();assert(root.ref==2);
 aux_fidexit(&other);assert(fid_server==&daemon&&fid_rootdir==&root&&root.ref==2);n++;
 struct fidmsg *held=malloc(sizeof *held);memset(held,0,sizeof *held);held->m_id=11;fid_cq.q_head=held;fid_cq.q_count=1;
 struct fidmsg *silent=malloc(sizeof *silent);memset(silent,0,sizeof *silent);silent->m_id=-1;held->m_next=silent;fid_cq.q_count++;
 aux_fidexit(&daemon);assert(!fid_server&&!fid_rootdir&&root.ref==1&&fid_cq.q_head==held&&!held->m_next&&fid_cq.q_count==1);n++;
 aux_fidexit(&daemon);assert(root.ref==1&&fid_cq.q_count==1);n++;
 aux_fidfree();assert(root.ref==1&&!fid_cq.q_count);n++;
 printf("PASS fid rename %d\n",n);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h+prod+t)
 subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu99','-Wall','-Wextra','-Wno-unused-parameter','-Wno-sign-compare',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
