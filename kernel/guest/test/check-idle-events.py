#!/usr/bin/env python3
import pathlib,sys,tempfile,subprocess
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('int\nui_getosevent(b)');b=s.index('/* UI_FLUSHEVENTS',a);f=s[a:b]
p=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef char *caddr_t;
struct uiev {int what,mods;long msg,when,where;};
struct uiev ev_q[4];int ev_n,ev_first,ev_wait,uin_evdiag;long uin_evstats[8],uin_evlast[8],lbolt,uin_mouse;short ev_rect[4];int forced,moved,signaled,in_btn,in_btnw,uin_nkey,uin_nbtn,uin_ndown,uin_nget;
#define NEV 4
#define DS_HI 1
#define PZERO 1
#define PCATCH 2
#define DS_SPL(x) 0
#define DS_SPLX(x) ((void)0)
#define G16(p) (((unsigned char *)(p))[0]*256+((unsigned char *)(p))[1])
#define G32(p) ((long)G16(p)*65536+G16((p)+2))
#define bcopy(a,b,n) memmove(b,a,n)
void ui_evbutton(int k,int w,int m){}
void ui_evtrace(void){}
int ui_forceget(void){return forced;}int ui_isnew(short*p){return moved;}int ui_mods(void){return 0;}void ui_btnout(int b){}void ev_put(char*b,struct uiev*e){}
int sleep(caddr_t p,int flags){if(signaled)return 1;lbolt=1000;return 0;}
void put(char*p,long v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
void reset(char*b){memset(b,0,32);memset(uin_evstats,0,sizeof uin_evstats);forced=moved=signaled=ev_n=0;lbolt=10;uin_evdiag=1;b[0]=1;put(b+20,2);}
'''
t=r'''
int main(void){char b[32];reset(b);uin_evdiag=0;b[0]=0;assert(ui_getosevent(b)==0&&uin_evstats[0]==0);
reset(b);b[0]=0;ui_getosevent(b);assert(uin_evstats[0]==1&&uin_evstats[3]==1&&uin_evstats[6]==0);
reset(b);put(b+20,0);ui_getosevent(b);assert(uin_evstats[4]==1);
reset(b);moved=1;ui_getosevent(b);assert(uin_evstats[5]==1);
reset(b);forced=1;ui_getosevent(b);assert(uin_evstats[1]==1);
reset(b);ev_n=1;ev_q[0].what=3;b[3]=8;ui_getosevent(b);assert(uin_evstats[2]==1&&ev_n==0);
reset(b);ui_getosevent(b);assert(uin_evstats[6]==1&&uin_evstats[4]==1&&uin_evlast[2]==2&&uin_evlast[3]==10&&uin_evlast[4]==12);
reset(b);signaled=1;ui_getosevent(b);assert(uin_evstats[6]==1&&uin_evstats[7]==1);
puts("idle-events: 8 production cases PASS");return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+t);o=pathlib.Path(d)/'t';subprocess.run(['cc','-std=gnu89','-fsanitize=undefined','-Wno-implicit-function-declaration',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
