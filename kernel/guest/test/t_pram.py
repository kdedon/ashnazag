#!/usr/bin/env python3
"""Exercise production PRAM ownership, staging and persistence separation."""
from pathlib import Path
import os
import subprocess
import tempfile

source=(Path(__file__).resolve().parents[1]/'mod/uinter/uinter.c').read_text()
start=source.index('static int\npramwrite(')
end=source.index('\nstatic int\nuimisc(',start)
production=source[start:end]
header='''#include <assert.h>
#include <string.h>
#include <stdio.h>
#define EPERM 1
#define EFAULT 14
#define EINVAL 22
#define LS_INUSE 2
typedef char *caddr_t;
struct cred { int cr_uid; };
struct proc { struct cred *p_cred; };
struct guest_proc { int id; };
static struct cred current={100},owner={100};
static struct proc proc={&owner};
static struct { struct cred *u_cred; int u_error; } u={&current,0};
static struct { int l_state; struct proc *l_proc; unsigned char l_pram[256]; } ui={2,&proc,{0}};
static unsigned char ui_pramsaved[256];
static int ui_pramdirty,attached=1,failcopy,copies;
static int ui_task(struct guest_proc *gp) { return attached && gp && gp->id==1; }
int suser(struct cred *cr) { if(cr->cr_uid) u.u_error=EPERM; return cr->cr_uid==0; }
static int copyin(caddr_t src,caddr_t dst,int n) {
 copies++; if(failcopy) { if(n) dst[0]=src[0]; return 1; } memcpy(dst,src,n); return 0;
}
#define bcopy(src,dst,n) memmove(dst,src,n)
'''
tests='''int main(void) {
 struct guest_proc gp={1}; unsigned char input[256],live[256],saved[256]; int i,n=0;
 memset(input,0xa5,sizeof input); memset(ui.l_pram,0x22,256); memset(ui_pramsaved,0x33,256);
 assert(pramwrite(&gp,(long)input,8,4)==0 && !ui_pramdirty && !u.u_error);
 assert(!memcmp(ui.l_pram+8,input,4) && ui_pramsaved[8]==0x33); n++;
 current.cr_uid=101; copies=0;
 assert(pramwrite(&gp,(long)input,8,4)==EPERM && !copies); n++;
 current.cr_uid=100; attached=0;
 assert(pramwrite(&gp,(long)input,8,4)==EPERM && !copies); n++;
 attached=1; ui.l_proc=0;
 assert(pramwrite(&gp,(long)input,8,4)==EPERM && !copies); n++;
 ui.l_proc=&proc; ui.l_state=0;
 assert(pramwrite(&gp,(long)input,8,4)==EPERM && !copies); n++;
 ui.l_state=LS_INUSE;
 assert(pramwrite(&gp,(long)input,-1,4)==EINVAL && !copies); n++;
 assert(pramwrite(&gp,(long)input,8,-1)==EINVAL && !copies); n++;
 assert(pramwrite(&gp,(long)input,257,0)==EINVAL && !copies); n++;
 assert(pramwrite(&gp,(long)input,255,2)==EINVAL && !copies); n++;
 for(i=0;i<2;i++) {
  current.cr_uid=i ? 0 : 100; failcopy=1;
  memcpy(live,ui.l_pram,256); memcpy(saved,ui_pramsaved,256);
  assert(pramwrite(&gp,(long)input,8,4)==EFAULT && !ui_pramdirty);
  assert(!memcmp(live,ui.l_pram,256) && !memcmp(saved,ui_pramsaved,256)); n++;
 }
 failcopy=0; current.cr_uid=0;
 assert(pramwrite(&gp,(long)input,8,4)==0 && ui_pramdirty);
 assert(!memcmp(ui_pramsaved+8,input,4)); n++;
 current.cr_uid=100; input[0]=0x77;
 assert(pramwrite(&gp,(long)input,8,1)==0 && ui.l_pram[8]==0x77 && ui_pramsaved[8]==0xa5); n++;
 current.cr_uid=0; failcopy=1;
 memcpy(live,ui.l_pram,256); memcpy(saved,ui_pramsaved,256);
 assert(pramwrite(&gp,(long)input,8,4)==EFAULT && ui_pramdirty);
 assert(!memcmp(live,ui.l_pram,256) && !memcmp(saved,ui_pramsaved,256)); n++;
 failcopy=0; ui_pramdirty=0;
 assert(pramwrite(&gp,(long)input,256,0)==0 && !ui_pramdirty); n++;
 attached=0;
 assert(pramwrite(&gp,(long)input,0,1)==EPERM); n++;
 printf("pram: %d checks passed\\n",n); return 0;
}
'''
assert 'vn_rdwr(UIO_WRITE, vp, (caddr_t)ui_pramsaved,' in source
with tempfile.TemporaryDirectory(prefix='pram-') as tmp:
    c,exe=Path(tmp)/'pram.c',Path(tmp)/'pram'
    c.write_text(header+production+tests)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu89','-Wall','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
