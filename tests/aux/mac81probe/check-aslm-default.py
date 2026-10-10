#!/usr/bin/env python3
import pathlib,re,subprocess,sys,tempfile
u=pathlib.Path(sys.argv[1]).read_text();t=pathlib.Path(sys.argv[2]).read_text()
assert re.search(r'\bint\s+uinter_aslmtrace\s*=\s*0\s*;',u)
match=re.search(r'\{\s*long trace=access\("/etc/aux/ASLMTraceProbe".*?\n        \}',t,re.S)
assert match
body=match.group(0)
assert t.index(body)<t.index('\n#ifdef SYS6\n\tsetsym("aux_trace"',t.index('startmac()'))
s='''#include <assert.h>
#include <string.h>
#include <stdio.h>
#define F_OK 0
static int present,badset,badget,observed,sets;static long value;
static int access(const char *p,int m) {assert(!strcmp(p,"/etc/aux/ASLMTraceProbe") && m==0);return present?0:-1;}
static int setsym(const char *p,long v) {assert(!strcmp(p,"uinter_aslmtrace"));sets++;if(badset)return -1;value=v;return 0;}
static long getsym(const char *p) {assert(!strcmp(p,"uinter_aslmtrace"));return badget?-1:value;}
static void t_check(const char *n,int ok,const char *fmt,...) {observed=!!ok;}
static void run(int marker,int failset,int failget,int want) {present=marker;badset=failset;badget=failget;value=!marker;sets=0;
'''+body+'''
assert(observed==want && sets==1);if(!failset)assert(value==marker);}
int main(void){int p;for(p=0;p<2;p++){run(p,0,0,1);run(p,1,0,0);run(p,0,1,0);}puts("6 source-extracted ASLM default/marker checks PASS");}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=pathlib.Path(tmp);(p/'test.c').write_text(s)
 subprocess.run(['cc','-O1','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
