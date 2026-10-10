#!/usr/bin/env python3
import os, pathlib, subprocess, sys
source=pathlib.Path(sys.argv[1]).read_text()
work=pathlib.Path(sys.argv[2]); work.mkdir(parents=True, exist_ok=True)
body=source[source.index('static int\notstream('):source.index('\nint\naux_ioctl(')]
assert 'return aux_otstream(a, rv, r);' in source
h=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef char *caddr_t;
typedef struct { long r_val1,r_val2; } rval_t;
struct vnode { int v_type,v_rdev; } node;
typedef struct { struct vnode *f_vnode; } file_t;
file_t file={&node};
struct { void *d_str; } cdevsw[70];
int cdevcnt=70, geterr, failin, failout, calls, native_error, checks;
unsigned char scratch[128];
#define VCHR 1
#define getmajor(d) (d)
#define OTX_TCP 1
#define OTX_UDP 2
#define OTX_ATTACH 0x6f01
#define I_PUSH 0x5302
#define I_FLUSH 0x5305
#define I_SRDOPT 0x5306
#define I_STR 0x5308
#define RMSGN 2
#define FLUSHR 1
#define FLUSHW 2
struct strioctl { int ic_cmd,ic_timout,ic_len; char *ic_dp; };
struct otx_attach { long at_session; unsigned long at_cookie; long at_proto; };
int getf(int fd,file_t **fp){if(fd!=9||geterr)return EBADF;*fp=&file;return 0;}
int copyin(char *a,char *b,int n){if(failin&&!--failin)return 1;memcpy(b,a,n);return 0;}
int copyout(char *a,char *b,int n){if(failout&&!--failout)return 1;memcpy(b,a,n);return 0;}
char *aux_gap(char *r,int n){(void)r;if(n>128)abort();return (char *)scratch;}
int aux_amix(int n,long *a,rval_t *rv){(void)a;(void)rv;if(n!=54)abort();calls++;return native_error;}
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line%d %s\n",__LINE__,#x);exit(1);}}while(0)
'''
t=r'''
int main(void){
 long a[3]={9,I_PUSH,0}; rval_t rv; char name[]="otxti"; int i;
 struct otx_attach at={23,45,OTX_TCP};
 struct strioctl io={OTX_ATTACH,0,sizeof at,(char *)&at};
 node.v_type=VCHR;node.v_rdev=33;cdevsw[33].d_str=&node;cdevsw[34].d_str=&node;
 a[2]=(long)name;CHECK(aux_otstream(a,&rv,0)==0);CHECK(calls==1);CHECK(a[2]==(long)scratch);CHECK(!strcmp((char *)scratch,"otxti"));
 name[0]='x';a[2]=(long)name;CHECK(aux_otstream(a,&rv,0)==EINVAL);name[0]='o';
 for(i=0;i<70;i++)if(i!=33&&i!=34){node.v_rdev=i;a[2]=(long)name;CHECK(aux_otstream(a,&rv,0)==EINVAL);}
 node.v_rdev=33;node.v_type=2;CHECK(aux_otstream(a,&rv,0)==EINVAL);node.v_type=VCHR;
 cdevsw[33].d_str=0;CHECK(aux_otstream(a,&rv,0)==EINVAL);cdevsw[33].d_str=&node;
 cdevcnt=33;CHECK(aux_otstream(a,&rv,0)==EINVAL);cdevcnt=70;
 geterr=1;CHECK(aux_otstream(a,&rv,0)==EINVAL);geterr=0;
 a[0]=8;CHECK(aux_otstream(a,&rv,0)==EINVAL);a[0]=9;
 a[1]=I_SRDOPT;a[2]=RMSGN;CHECK(aux_otstream(a,&rv,0)==0);a[2]=1;CHECK(aux_otstream(a,&rv,0)==EINVAL);
 a[1]=I_FLUSH;for(i=0;i<8;i++){a[2]=i;CHECK(aux_otstream(a,&rv,0)==(i>=1&&i<=3?0:EINVAL));}
 a[1]=I_STR;a[2]=(long)&io;CHECK(aux_otstream(a,&rv,0)==0);
 {struct strioctl *c=(void *)scratch;struct otx_attach *p=(void *)c->ic_dp;CHECK(c->ic_dp==(char *)scratch+sizeof io);CHECK(c->ic_cmd==OTX_ATTACH);CHECK(c->ic_len==sizeof at);CHECK(p->at_cookie==45);at.at_cookie=46;CHECK(p->at_cookie==45);at.at_cookie=45;}
 for(i=1;i<=2;i++){a[2]=(long)&io;failin=i;CHECK(aux_otstream(a,&rv,0)==EFAULT);}failin=0;
 for(i=1;i<=2;i++){a[2]=(long)&io;failout=i;CHECK(aux_otstream(a,&rv,0)==EFAULT);}failout=0;
 #define BAD(field,value) do {long old=field;field=value;a[2]=(long)&io;CHECK(aux_otstream(a,&rv,0)==EINVAL);field=old;}while(0)
 BAD(io.ic_cmd,123);BAD(io.ic_timout,-1);BAD(io.ic_timout,1);BAD(io.ic_len,0);BAD(io.ic_len,sizeof at+1);BAD(at.at_session,0);BAD(at.at_session,-1);BAD(at.at_cookie,0);BAD(at.at_proto,OTX_UDP);
 node.v_rdev=34;at.at_proto=OTX_UDP;a[2]=(long)&io;CHECK(aux_otstream(a,&rv,0)==0);
 node.v_rdev=33;at.at_proto=OTX_TCP;native_error=EPERM;a[2]=(long)&io;CHECK(aux_otstream(a,&rv,0)==EPERM);native_error=0;
 a[1]=123;CHECK(aux_otstream(a,&rv,0)==EINVAL);
 a[1]=I_PUSH;a[2]=(long)name;failin=1;CHECK(aux_otstream(a,&rv,0)==EFAULT);failout=1;CHECK(aux_otstream(a,&rv,0)==EFAULT);
 printf("PASS %d authenticated STREAMS ioctl checks\n",checks);return 0;
}
'''
p=work/'fixture.c';p.write_text(h+body+t)
subprocess.run(['cc','-std=gnu89','-O','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(work/'fixture')],check=True)
subprocess.run([str(work/'fixture')],check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
