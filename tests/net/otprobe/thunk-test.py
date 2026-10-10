#!/usr/bin/env python3
"""Check the production thunk guard and read-only address resolution."""
from pathlib import Path
import os, subprocess, sys, tempfile
s=(Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).with_name('probe.c')).read_text()
f=s[s.index('static int relativeaddress('):s.index('static void endpointinfo(')]
code=r'''#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char memory[65536];
static int reads,targets,returns,globals;static unsigned long lasttarget;
static void out(char *s) {(void)s;}
static void hex(unsigned long x) {(void)x;}
static unsigned long byteslong(unsigned char *p) {return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];}
static int snapshot(unsigned long a,void *p,unsigned long n) {reads++;if(a>=sizeof memory || n>sizeof memory-a)return 0;memcpy(p,memory+a,n);return 1;}
static void codeinfo(char *s,unsigned long a) {
 if(!strcmp(s,"endpoint thunk target")){targets++;lasttarget=a;}
 if(!strcmp(s,"endpoint thunk return"))returns++;
 if(!strcmp(s,"endpoint thunk A5"))globals++;
}
''' + f + r'''
static unsigned char original[]={0x41,0xfa,0xff,0x94,0x22,0x50,0x50,0x90,0x22,0xdf,0x22,0x8d,0x2a,0x60,0x48,0x7a,0xff,0xa0,0x60,0x00,0x01,0x6e};
static void reset(void) {memset(memory,0,sizeof memory);reads=targets=returns=globals=0;memcpy(memory+0x3000,original,sizeof original);}
int main(void) {
 int i;unsigned long a;
 reset();thunkinfo(0x3000);assert(reads==3&&targets==1&&returns==1&&globals==1&&lasttarget==0x3182);
 for(i=0;i<22;i++){reset();memory[0x3000+i]^=1;thunkinfo(0x3000);assert(reads==1&&!targets&&!returns&&!globals);}
 reset();thunkinfo(0xfffffff0UL);assert(!reads&&!targets);
 reset();thunkinfo(0x10000);assert(reads==1&&!targets);
 reset();memcpy(memory+1024,original,sizeof original);thunkinfo(1024);assert(reads==1&&!targets);
 reset();memcpy(memory+0x3001,original,sizeof original);thunkinfo(0x3001);assert(reads==1&&!targets);
 assert(relativeaddress(0x77df50,-108,&a)&&a==0x77dee4);
 assert(relativeaddress(0x77df62,366,&a)&&a==0x77e0d0);
 assert(relativeaddress(0x77df5e,-96,&a)&&a==0x77defe);
 assert(!relativeaddress(0xfffffffeUL,2,&a));
 assert(!relativeaddress(0,-1,&a));
 assert(!relativeaddress(1024,-2,&a));
 assert(!relativeaddress(1024,1,&a));
 puts("PASS 34 production thunk signature, bounds and PC-relative cases");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='endpoint-thunk-') as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
