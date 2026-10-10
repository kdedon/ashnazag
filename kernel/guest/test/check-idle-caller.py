#!/usr/bin/env python3
import pathlib,sys,tempfile,subprocess
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('long uin_evtracecount');z=s.index('/* UI_GETOSEVENT',a);f=s[a:z]
p=r'''
#include <assert.h>
#include <string.h>
#include <stdint.h>
typedef char *caddr_t;unsigned char memory[65536];
struct {char *u_ar0;}u;long savedpc,savedusp,savedfp,lbolt=99;int in_flags=7,in_btn=0,in_btnw=0,ev_n=3,failed,lines;struct {int l_evmask;}ui;
#define GR_PC(r) savedpc
#define GR_USP(r) savedusp
#define GR_A(r,n) savedfp
#define LM_MBSTATE 0x172
#define G32(p) (((uint32_t)(unsigned char)(p)[0]<<24)|((uint32_t)(unsigned char)(p)[1]<<16)|((uint32_t)(unsigned char)(p)[2]<<8)|(unsigned char)(p)[3])
#define bzero(p,n) memset(p,0,n)
int copyin(caddr_t a,caddr_t b,int n){uintptr_t x=(uintptr_t)a;if(failed||x+n>sizeof memory)return 1;memcpy(b,memory+x,n);return 0;}
int fubyte(caddr_t a){return 128;}
int printf(char*f,...){lines++;return 0;}
void put(int a,int v){memory[a]=v>>24;memory[a+1]=v>>16;memory[a+2]=v>>8;memory[a+3]=v;}
void reset(void){memset(memory,0,sizeof memory);uin_evtracecount=0;savedusp=0x2000;savedfp=0x3000;savedpc=0x1001;failed=lines=0;}
'''
# reset uses the extracted globals.
p=p.replace('void reset(void)', 'void reset(void)')
a=p.index('void reset(void)');reset=p[a:];p=p[:a]
t=r'''
int main(void){int i;reset();put(0x3000,0x3100);put(0x3100,0x3200);ui_evtrace();assert(uin_evtrace[0][28]==1&&uin_evtrace[0][29]==3&&uin_evtrace[0][3]==128);
reset();savedusp=0x1000000;ui_evtrace();assert(!uin_evtrace[0][28]);
reset();savedfp=0x3001;ui_evtrace();assert(!uin_evtrace[0][29]);
reset();savedfp=0x1000000;ui_evtrace();assert(!uin_evtrace[0][29]);
reset();failed=1;ui_evtrace();assert(!uin_evtrace[0][28]&&!uin_evtrace[0][29]);
reset();put(0x3000,0x3000);ui_evtrace();assert(uin_evtrace[0][29]==1);
reset();for(i=0;i<10;i++)ui_evtrace();assert(uin_evtracecount==4&&lines==128);uin_evtracecount=-1;lines=0;ui_evtrace();assert(uin_evtracecount==-1&&!lines);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+reset+t);o=pathlib.Path(d)/'t';subprocess.run(['cc','-std=gnu89','-fno-builtin','-fsanitize=undefined',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
print('idle-caller: 8 production snapshot cases PASS')
