from pathlib import Path
import subprocess,sys,os
r=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
s=(r/'otbst.c').read_text();s='\n'.join(x for x in s.splitlines() if not x.startswith('#include') and not x.startswith('asm('))
pre=r'''
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include "otwire.h"
#include "vstation.h"
#include "anchor.h"
#define OTB_STMINOR 64
#define getminor(x) (x)
#define getmajor(x) 0
#define makedevice(x,y) (y)
#define OTB_SPLSTR(x) ((x)=0)
#define OTB_SPLX(x) ((void)(x))
#define BPRI_MED 1
#define FNDELAY 1
#define FNONBLOCK 2
#define PZERO 0
#define PCATCH 0
#define UIO_READ 0
#define UIO_WRITE 1
#define NSIG 32
#define POLLIN 1
#define POLLOUT 4
#define POLLRDNORM 64
#ifndef ENOSR
#define ENOSR 63
#endif
struct pollhead {int unused;};
struct proc {int p_pid;};
struct proc current={3},*curproc=&current;
typedef struct {int cr_uid;} cred_t;
struct uio {int uio_resid,uio_fmode;unsigned char *bytes;};
typedef struct block {struct block*b_next;unsigned char*b_rptr,*b_wptr;} mblk_t;
struct vst_ops *vst_ops;
static int live,allocation,copyfail,wakes,signals;
mblk_t *allocb(int n,int priority) {mblk_t*m;(void)priority;if(allocation)return 0;m=calloc(1,sizeof(*m)+n);assert(m);m->b_rptr=m->b_wptr=(unsigned char*)(m+1);live++;return m;}
void freemsg(mblk_t*m){assert(m&&live);live--;free(m);}
int copyin(char*s,char*d,int n){if(copyfail)return 1;memcpy(d,s,n);return 0;}
int copyout(char*s,char*d,int n){return copyin(s,d,n);}
int groupmember(int gid,cred_t*c){return gid==25&&c->cr_uid==25;}
int sleep(char*p,int pri){(void)p;(void)pri;return 1;}
void delay(int n){(void)n;}
void wakeup(char*p){(void)p;wakes++;}
void pollwakeup(struct pollhead*p,int events){(void)p;(void)events;}
void psignal(struct proc*p,int sig){(void)p;(void)sig;signals++;}
int uiomove(char*p,long n,int mode,struct uio*u){if(copyfail)return EFAULT;if(mode==UIO_WRITE)memcpy(p,u->bytes,n);else memcpy(u->bytes,p,n);u->uio_resid-=n;return 0;}
'''
tail=r'''
static int checks;
#define CHECK(x) do{assert(x);checks++;}while(0)
int main(void){cred_t root={0},user={25},other={26};dev_t d=OTB_STMINOR,d2=OTB_STMINOR;int rv=9,j;struct otb_identity i,g;struct otb_station st;unsigned char pkt[42],out[60];struct uio u;short events;struct pollhead*ph=0;
 memset(&i,0,sizeof i);i.version[3]=1;memcpy(i.address,"\012\000\002\017",4);memcpy(i.mask,"\377\377\377\000",4);memcpy(i.router,"\012\000\002\002",4);
 CHECK(otbst_identity(OTB_GETIDENT,(char*)&g,&user)==ENXIO);
 CHECK(otbst_identity(OTB_SETIDENT,(char*)&i,&user)==EPERM);
 copyfail=1;CHECK(otbst_identity(OTB_SETIDENT,(char*)&i,&root)==EFAULT);copyfail=0;
 g=i;g.version[3]=2;CHECK(otbst_identity(OTB_SETIDENT,(char*)&g,&root)==EINVAL);
 CHECK(otbst_identity(OTB_SETIDENT,(char*)&i,&root)==0);
 CHECK(otbst_identity(OTB_GETIDENT,(char*)&g,&user)==0&&!memcmp(&i,&g,sizeof i));
 CHECK(otbst_open(&d2,&other)==EPERM);CHECK(otbst_open(&d,&user)==0&&otb_nst==1);
 d2=OTB_STMINOR;CHECK(otbst_open(&d2,&user)==EBUSY);
 memset(&st,0,sizeof st);st.st_mode=OTB_ST_BRIDGE;CHECK(otbst_ioctl(d,OTB_STATION,(char*)&st,&user,&rv)==ENXIO);
 st.st_mode=OTB_ST_ANCHOR;st.st_mac[0]=2;CHECK(otbst_ioctl(d,OTB_STATION,(char*)&st,&user,&rv)==EPERM);
 memset(st.st_mac,0,6);CHECK(otbst_ioctl(d,OTB_STATION,(char*)&st,&user,&rv)==0&&rv==0);
 CHECK(st.st_mac[0]==2&&st.st_mac[5]==25&&otb_st_[d-OTB_STMINOR].t_vst==-1);
 CHECK(otbst_ioctl(d,OTB_STATION,(char*)&st,&user,&rv)==EBUSY);
 CHECK(otbst_poll(d,POLLIN|POLLOUT,0,&events,&ph)==0&&events==POLLOUT);
 memset(pkt,0,sizeof pkt);memcpy(pkt+6,st.st_mac,6);pkt[12]=8;pkt[13]=6;pkt[15]=1;pkt[16]=8;pkt[18]=6;pkt[19]=4;pkt[21]=1;memcpy(pkt+22,st.st_mac,6);memcpy(pkt+28,i.address,4);memcpy(pkt+38,i.router,4);
 for(j=0;j<9;j++){u.bytes=pkt;u.uio_resid=42;u.uio_fmode=FNONBLOCK;CHECK(otbst_write(d,&u)==0);}
 CHECK(otb_st_[d-OTB_STMINOR].t_n==8&&live==8&&otb_st_[d-OTB_STMINOR].t_st.ss_drop==1);
 CHECK(otbst_poll(d,POLLIN|POLLOUT,0,&events,&ph)==0&&events==(POLLIN|POLLOUT));
 u.bytes=out;u.uio_resid=sizeof out;u.uio_fmode=FNONBLOCK;CHECK(otbst_read(d,&u)==0&&out[21]==2&&live==7);
 copyfail=1;u.bytes=pkt;u.uio_resid=42;CHECK(otbst_write(d,&u)==EFAULT&&live==7);copyfail=0;
 allocation=1;u.uio_resid=42;CHECK(otbst_write(d,&u)==ENOSR&&live==7);allocation=0;
 CHECK(otbst_close(d)==0&&otb_nst==0&&live==0);
 CHECK(otbst_read(d,&u)==ENXIO);CHECK(otbst_poll(d,POLLOUT,0,&events,&ph)==ENXIO);
 printf("PASS %d production station permission/queue/lifetime checks\n",checks);return 0;}
'''
p=out/'station-production.c';p.write_text(pre+s+tail)
subprocess.run(['nice','-n19','cc','-std=gnu89','-w','-O1','-g','-fsanitize=address,undefined','-I'+str(r),str(p),str(r/'anchor.c'),'-o',str(out/'station-test')],check=True)
subprocess.run([str(out/'station-test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
