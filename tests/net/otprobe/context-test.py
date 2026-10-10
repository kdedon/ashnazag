#!/usr/bin/env python3
"""Exercise guarded read-only endpoint snapshots from the production probe."""
from pathlib import Path
import re,subprocess,sys,tempfile,os
s=(Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).with_name('probe.c')).read_text()
f=s[s.index('static int relativeaddress('):s.index('static void objectinfo(')]
body=re.search(r'signature\[\] = \{(.*?)\};',f,re.S).group(1)
sig=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]+)',body))
assert len(sig)==60
p=Path(tempfile.mkdtemp(prefix='endpoint-info-',dir='/tmp'))
code=r'''#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
static unsigned char memory[0x10000];
static int contexts,slots,innercodes,failcode;
static unsigned long lastcode;
static void out(char *s) {(void)s;}
static void hex(unsigned long v) {(void)v;}
static unsigned long byteslong(unsigned char *p) {return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];}
static void put(unsigned long a,unsigned long v) {memory[a]=v>>24;memory[a+1]=v>>16;memory[a+2]=v>>8;memory[a+3]=v;}
static int snapshot(unsigned long a,void *b,unsigned long n) {
 if(a==0x2000-64)contexts++;if(a==0x2000-112)slots++;
 if((failcode&&a==0x1000)||a>=sizeof memory||n>sizeof memory-a)return 0;
 memcpy(b,memory+a,n);return 1;
}
static void codeinfo(char *name,unsigned long a) {if(!strcmp(name,"endpoint inner code")){innercodes++;lastcode=a;}}
''' + f + '\nstatic unsigned char original[]={' + ','.join(str(v) for v in sig) + '};\n'
code+=r'''
static void reset(void) {
 memset(memory,0,sizeof memory);contexts=slots=innercodes=failcode=0;
 put(0x800,0x1000);put(0x804,0x2000);memcpy(memory+0x1000,original,sizeof original);
 put(0x2000-64,0x2800);put(0x2000-112,0x3000);
 memory[0x3000]=0xaa;memory[0x3001]=0xfe;memory[0x3002]=7;put(0x3014,0x4500);
}
int main(void) {
 int i;
 reset();endpointinfo(0x800);assert(contexts==1&&slots==1&&innercodes==1&&lastcode==0x4500);
 for(i=0;i<sizeof original;i++) {reset();memory[0x1000+i]^=1;endpointinfo(0x800);assert(!contexts&&!slots&&!innercodes);}
 reset();endpointinfo(0x10000);assert(!contexts&&!slots&&!innercodes);
 reset();failcode=1;endpointinfo(0x800);assert(!contexts&&!slots&&!innercodes);
 reset();put(0x804,111);endpointinfo(0x800);assert(!contexts&&!slots&&!innercodes);
 reset();memory[0x3002]=8;endpointinfo(0x800);assert(!innercodes);
 reset();memory[0x300b]=1;endpointinfo(0x800);assert(!innercodes);
 reset();memory[0x3011]=1;endpointinfo(0x800);assert(!innercodes);
 reset();memory[0x3011]=0x10;memory[0x3013]=4;put(0x3014,0x4000);put(0x4000,0x4600);put(0x4004,0x4800);
 endpointinfo(0x800);assert(innercodes==1&&lastcode==0x4600);
 puts("PASS 68 production snapshot guard/refusal/direct-code/CFM-vector cases");return 0;
}
'''
try:
 (p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
finally:
 for item in p.iterdir():item.unlink()
 p.rmdir()
