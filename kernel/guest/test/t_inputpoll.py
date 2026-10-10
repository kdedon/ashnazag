#!/usr/bin/env python3
"""Check the modal input bridge's wait, interruption and teardown paths."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'kernel/guest/mod/uinter/uievent.c').read_text()
s=s[s.index('long uin_pollmouse,'):].replace('long until','int32_t until').replace('(long)(lbolt-until)','(int32_t)((uint32_t)lbolt-(uint32_t)until)').replace('lbolt+1','(uint32_t)lbolt+1')
header='''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef char *caddr_t;
#define DS_HI 7
#define DS_SPL(x) (assert((x)==DS_HI),0)
#define DS_SPLX(x) ((void)(x))
#define LM_MTEMP 0x828
#define LM_MBSTATE 0x172
#define LM_CRSRVIS 0x8cc
#define G32(b) ((uint32_t)(b)[0]<<24 | (uint32_t)(b)[1]<<16 | (uint32_t)(b)[2]<<8 | (b)[3])
static int in_btn,in_mv=218,in_mh=588;
static unsigned char memory[4096];
static int copyin(caddr_t src,caddr_t dst,int n) {
 uintptr_t p=(uintptr_t)src; assert(p+n<=sizeof memory); memcpy(dst,memory+p,n);return 0;
}
#define PZERO 10
#define PCATCH 32
#define EINTR 4
#define EINVAL 22
struct guest_proc { int x; };
static struct { int l_tid; struct guest_proc *l_active; } ui;
static int32_t lbolt;
static int waits,updates,interrupted,stop;
static int sleep(caddr_t addr,int pri) {
 assert(addr==(caddr_t)&ui.l_tid && pri==((PZERO+1)|PCATCH)); waits++;
 lbolt=(uint32_t)lbolt+1;
 if(stop) ui.l_tid=0;
 return interrupted;
}
static void ui_update(struct guest_proc *gp) { assert(gp==ui.l_active); updates++; }
'''
tests='''int main(void) {
 struct guest_proc a,b;
 ui.l_tid=1;ui.l_active=&a;lbolt=10;
 memory[LM_MTEMP+8]=0; memory[LM_MTEMP+9]=111;
 memory[LM_MTEMP+10]=0; memory[LM_MTEMP+11]=202;
 memory[LM_MBSTATE]=128; memory[LM_CRSRVIS+1]=1;
 memory[LM_CRSRVIS+3]=255;
 assert(!ui_inputpoll(&a) && waits==1 && updates==1);
 assert(uin_pollmouse==0x006f00ca && uin_pollhost==0x00da024c);
 assert(uin_pollmb==128 && uin_pollcursor==0x000100ff);
 assert(uin_polledges==1 && uin_pollup==1 && !uin_polldown);
 in_btn=1;memory[LM_MBSTATE]=0;
 lbolt=INT32_MAX;assert(!ui_inputpoll(&a) && waits==2 && updates==2);
 assert(uin_polledges==2 && uin_polldown==1 && uin_pollmb==0);
 assert(uin_polldownmouse==0x006f00ca && uin_polldownhost==0x00da024c &&
        uin_polldowncursor==0x000100ff && !uin_polldownmb);
 assert(ui_inputpoll(&b)==EINVAL && waits==2 && updates==2);
 interrupted=1;assert(ui_inputpoll(&a)==EINTR && waits==3 && updates==3);
 interrupted=0;stop=1;assert(ui_inputpoll(&a)==EINVAL && waits==4 && updates==3);
 assert(ui_inputpoll(&a)==EINVAL && waits==4 && updates==3);
 puts("inputpoll: 11 production checks passed");return 0;
}'''
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/'t.c';out=Path(d)/'t';c.write_text(header+s+tests)
 subprocess.run(['cc','-std=gnu89','-Wall','-Werror',str(c),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
