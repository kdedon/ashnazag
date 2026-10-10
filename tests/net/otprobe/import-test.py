#!/usr/bin/env python3
"""Exercise the guarded production lazy-import snapshot."""
from pathlib import Path
import os,subprocess,sys,tempfile
s=(Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).with_name('probe.c')).read_text()
f=s[s.index('static void importinfo('):s.index('static void thunkinfo(')]
code=r'''#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned char memory[65536];static int reads,entries,records,failbinding;static unsigned long lastentry,lastrecord;
static void out(char *s){(void)s;}
static void hex(unsigned long v){(void)v;}
static unsigned long byteslong(unsigned char *p){return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];}
static void put(unsigned long a,unsigned long v){memory[a]=v>>24;memory[a+1]=v>>16;memory[a+2]=v>>8;memory[a+3]=v;}
static int snapshot(unsigned long a,void *p,unsigned long n){reads++;if((failbinding&&a==0x2000-202)||a>=sizeof memory||n>sizeof memory-a)return 0;memcpy(p,memory+a,n);return 1;}
static void codeinfo(char *s,unsigned long a){if(!strcmp(s,"endpoint import record")){records++;lastrecord=a;}if(!strcmp(s,"endpoint resolved entry")){entries++;lastentry=a;}}
''' + f + r'''
static const unsigned char original[]={0x41,0xed,0xff,0x36,0x20,0x10,0x67,0x04,0x20,0x40,0x4e,0xd0,0x4e,0xfa,0,0x66};
static void reset(void){memset(memory,0,sizeof memory);reads=entries=records=failbinding=0;memcpy(memory+0x3000,original,16);put(0x2000-202,0x4000);put(0x2000-198,0x5000);put(0x2000-192,7);}
int main(void){int i;
 reset();importinfo(0x3000,0x2000);assert(reads==2&&entries==1&&lastentry==0x4000&&records==1&&lastrecord==0x5000);
 for(i=0;i<16;i++){reset();memory[0x3000+i]^=1;importinfo(0x3000,0x2000);assert(reads==1&&!entries&&!records);}
 reset();importinfo(0x10000,0x2000);assert(reads==1&&!records);
 reset();importinfo(0x3000,1225);assert(reads==1&&!records);
 reset();importinfo(0x3000,0x2001);assert(reads==1&&!records);
 reset();importinfo(0x3000,0x100000000UL);assert(reads==1&&!records);
 reset();put(0x2000-202,0);importinfo(0x3000,0x2000);assert(reads==2&&!entries&&records==1);
 reset();failbinding=1;importinfo(0x3000,0x2000);assert(reads==2&&!entries&&!records);
 assert(0x77e028UL-202==0x77df5eUL);
 puts("PASS 24 production import guard, bounds, unresolved/resolved and refusal cases");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='endpoint-import-') as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
