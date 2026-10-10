#!/usr/bin/env python3
import pathlib, re, subprocess, sys, tempfile

u = pathlib.Path(sys.argv[1]).read_text()
t = pathlib.Path(sys.argv[2]).read_text()
assert re.search(r'\buinter_fmgroup\s*=\s*0\s*;', u)
table = u.split('} uipatches[] = {', 1)[1].split('\n};', 1)[0]
entries = re.findall(r'^\s*\{.*?\}', table, re.M)
assert [i for i, e in enumerate(entries) if ', PGROUP,' in e] == [6, 7]
assert ', PHFSINIT,' in entries[0] and ', PHFS,' in entries[12]
assert 'if ((uipatches[i].p_kind == PGROUP && !uinter_fmgroup)' in u
assert 'uinter_patched |= 1L << tw;' in u
start = t.index('t_check("live_patches",')
end = t.index(';', start) + 1
call = t[start:end]
prefix = '''#include <assert.h>
#include <stdio.h>
#include <string.h>
static long fm, patches; static int observed;
static long getsym(const char *s) { return !strcmp(s,"uinter_fmgroup") ? fm : patches; }
static void t_check(const char *n,int ok,const char *f,...) { observed=!!ok; }
static void run(long f,long p,int want) { fm=f; patches=p;
'''
suffix = '''assert(observed==want); }
int main(void) { int f,i,j,n=0; long base;
 for(f=0;f<=1;f++) { base=f?0x117f:0x113f;
  for(j=0;j<16;j++) { long optional=0; int bits[]={9,10,11,13};
   for(i=0;i<4;i++)if(j&(1<<i))optional|=1L<<bits[i];
   run(f,base|optional,1); n++;
   for(i=0;i<14;i++)if(!(0x2e00L&(1L<<i))) {run(f,(base|optional)^(1L<<i),0);n++;}
  }
 }
 run(-1,0x117f,0); n++; run(0,0x3b3f,1);n++;
 printf("%d native FileManager mask checks PASS\\n",n); return 0; }
'''
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d); (p/'test.c').write_text(prefix+call+suffix)
 subprocess.run(['cc','-O1','-fsanitize=undefined','-o',str(p/'test'),str(p/'test.c')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Production default, PGROUP indices, PHFS dependencies PASS')
