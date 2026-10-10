#!/usr/bin/env python3
"""Exercise screenshot comparison requests at their buffer boundary."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root/'tests/src/t_mac.c'
text = source.read_text()
start = text.index('static int\nshotdiff(a, b)')
end = text.index('\n/* the Mac still running:', start)
code = '''#include <assert.h>
#include <stdio.h>
#include <string.h>
static int calls;
static char request[256];
static char *host(char *s) { calls++; strcpy(request,s); return "diff 256"; }
''' + text[start:end] + r'''
int main(void) {
 char a[124], b[124];
 assert(shotdiff("mac81_before_repeat_forcequit","mac81_repeat_forcequit_cancelled")==256);
 assert(!strcmp(request,"cmp mac81_before_repeat_forcequit mac81_repeat_forcequit_cancelled"));
 memset(a,'a',61); a[61]=0; memset(b,'b',61); b[61]=0;
 assert(shotdiff(a,b)==256 && strlen(request)==127);
 b[61]='b'; b[62]=0;
 assert(shotdiff(a,b)==-1 && calls==2);
 memset(a,'a',123); a[123]=0;
 assert(shotdiff(a,"x")==-1 && calls==2);
 puts("shotdiff: repeat names, exact boundary and two oversized requests PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='shotdiff-') as tmp:
 p=Path(tmp); (p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),check=True)
