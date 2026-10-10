#!/usr/bin/env python3
"""Exercise the production notifier retry with deterministic time and errors."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / 'probe.c').read_text()
start = source.index('static long install_notifier(')
end = source.index('\nstatic ', start + 1)
helper = source[start:end]
harness = r'''
#include <assert.h>
#include <stdio.h>
static unsigned long clock_value, clock_step, calls, idles, succeed;
static long failure;
static void *expected_desc, *expected_idle;
static long tickcount(void) { unsigned long n=clock_value; clock_value+=clock_step; return n; }
static long invoke(void *d,int n,long ep,long notifier,long context,long zero) {
 assert(d==expected_desc && n==3 && ep==7 && notifier==8 && context==9 && zero==0);
 ++calls; return succeed && calls>=succeed ? 0 : failure;
}
static void idle(void *d) { assert(d==expected_idle); ++idles; }
static void out(const char *s) { (void)s; }
static void hex(unsigned long n) { (void)n; }
HELPER
static void reset(long error,unsigned long success,unsigned long step) {
 failure=error; succeed=success; clock_step=step; clock_value=calls=idles=0;
 expected_desc=(void *)1; expected_idle=(void *)2;
}
static long run(void) { return install_notifier(expected_desc,expected_idle,7,8,9); }
int main(void) {
 reset(-3168,1,1); assert(run()==0 && calls==1 && idles==0);
 reset(-3168,4,1); assert(run()==0 && calls==4 && idles==3);
 reset(-42,0,1); assert(run()==-42 && calls==1 && idles==0);
 reset(-3168,0,100); assert(run()==-3168 && calls==6 && idles==5);
 reset(-3168,0,0); assert(run()==-3168 && calls==100000 && idles==99999);
 reset(-3168,3,1); clock_value=~0UL-1; assert(run()==0 && calls==3 && idles==2);
 puts("PASS 6 production notifier retry/error/time/spin/wrap cases");
 return 0;
}
'''.replace('HELPER', helper)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness)
    subprocess.run(['cc', '-std=gnu89', '-O1', '-g', '-Wall', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    subprocess.run([str(path / 'test')], check=True, env=env)
