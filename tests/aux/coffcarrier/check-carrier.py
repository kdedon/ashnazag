#!/usr/bin/env python3
"""Exercise admission and cleanup in the native carrier source."""
import pathlib,subprocess,tempfile,sys,struct,re
root=pathlib.Path(__file__).resolve().parents[3]
src=(root/"tests/aux/coffcarrier/carrier.c").read_text()
mock=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
static long stackcontextwords[16];
long gettooltrap(long t) {assert(t==0xa8fd || t==0xa9b4 || t==0xa9ea || t==0xa82e);return 0x20000+2*(t&255);}
void settooltrap(long t,long p) {assert(0);}
long auxsys4(long n,long a,long b,long c,long d) {assert(0);return -38;}
long stackcontext(void) {return (long)stackcontextwords;}
void setstackbase(long v) {assert(0);}
static char *inherited[]={"HOME=/home/test user","TBSYSTEM=/home/test user/Mac/8.1/System Folder","TBMEMORY=24M","CUSTOM=kept",0};
static int envfail;
static long forkresult=24, switcherror,hold,version=0x0810;
static int waits,switches,kills,reaps,exits,execs,next,identity,cooperative,waiterror;
long auxwait(long pid,long *status,long options) {
 assert(pid==24 && (options==0 || options==1) && hold==(32|(1L<<17)));
 if(waiterror)return -10;
 if(options==1 && (switcherror || cooperative)) {if(cooperative)cooperative--;return 0;}
 reaps++;*status=kills?9:0;return pid;
}
long systemversion(void) {return version;}
long currenta5(void) { return 0x123456; }
long auxspawn(char *raw,long mask) {assert(mask==32);*(uint32_t *)raw=forkresult;return forkresult;}
long getnext(char *p) { if(next++) return -50; *(long *)p=0; return 0; }
long getcurrent(char *p) {*(int32_t *)p=0;*(int32_t *)(p+4)=8193;return identity==1?-50:0;}
long getprocess(char *p,char *i) {
 *(int32_t *)(i+8)=*(int32_t *)p;*(int32_t *)(i+12)=*(int32_t *)(p+4);
 *(int32_t *)(i+20)=identity==2?0:0x434f6666;
 if(identity==3)*(int32_t *)(i+12)=8194;
 *(int32_t *)(i+28)=0x120000;*(int32_t *)(i+32)=0x10000;
 return identity==4?-50:0;
}
void waitnext(char *p) { waits++; }
void exitapp(void) { exits++; }
long auxwrite(void) { return 0; }
long auxsysr(long n,long a,long b,long c) {
 if(n==130) return 32;
 if(n==131) {assert(a==32 || a==(32|(1L<<17)));hold=a;return 0;}
 if(n==20) return 20;
 if(n==5) return 3;
 if(n==54 && b==0x40085195L) {if(envfail)return -14;((long *)c)[0]=(long)inherited;((long *)c)[1]=4;return 0;}
 if(n==54 && b==0xc0145190L) {assert(a==3 && ((long *)c)[0]==24 && ((long *)c)[1]==0 && ((long *)c)[2]==8193 && ((long *)c)[3]==0x123456);((long *)c)[4]=1;return 0;}
 if(n==54) {assert(a==3 && b==0x80045126L && *(long *)c==24);switches++;return switcherror;}
 if(n==37) {assert(a==24 && b==9);kills++;return 0;}
 if(n==151) {assert(a==24 && c==0);reaps++;*(long *)b=9;return 24;}
 if(n==59) {assert(!strcmp((char *)a,"/mac/bin/CommandShell"));assert((char **)c==inherited && !strcmp(((char **)c)[3],"CUSTOM=kept"));execs++;return -2;}
 if(n==1) {assert(a==127);return 0;}
 return 0;
}
void reset(void) {memset(stackcontextwords,0,sizeof stackcontextwords);stackcontextwords[0]=0x70000000L;stackcontextwords[2]=0x123456;waits=switches=kills=reaps=exits=execs=next=0;}
'''
for path in re.findall(r'"(/[^"\n]*)"',src):
    assert all(len(component.encode())<=14 for component in path.split('/')),path
stage=(root/'tests/aux/build.sh').read_text()
block=stage.split('if [ -n "${COFFCARRIER:-}${COFFADAPTER:-}" ]; then',1)[1].split('    fi',1)[0]
for path in re.findall(r'"\$OUT/root(/[^"\n]*)"',block):
    assert all(len(c.encode())<=14 for c in path.split('/')),path
harness=(root/'tests/src/t_mac.c').read_text()
block=harness.split('commandcarrier81()',1)[1].split('static void\ncommandunix81',1)[0]
for path in re.findall(r'"(/[^"\n]*)"',block):
    assert all(len(c.encode())<=14 for c in path.split('/')),path
print('carrier: S5 path components PASS')
main=r''' 
int main(void) {
 reset();forkresult=24;switcherror=0;start();
 assert(switches==1 && !waits && !kills && reaps==1 && exits==1);
 reset();switcherror=-22;start();
 assert(switches==300 && waits==300 && kills==1 && reaps==1 && exits==1);
 reset();switcherror=-1;start();
 assert(switches==1 && !waits && kills==1 && reaps==1);
 reset();forkresult=-11;start();assert(!switches && !kills && !reaps && exits==1);
 reset();cooperative=2;forkresult=24;switcherror=0;start();assert(switches==3&&!kills&&reaps==1&&exits==1);
 reset();waiterror=1;start();assert(switches==1&&!kills&&!reaps&&exits==1);waiterror=0;
 reset();coffenvironment=inherited;childexec(32);assert(execs==1 && !switches && !kills && !reaps);
 assert(hold==32);
 reset();version=0x0761;childexec(32);assert(execs==1 && hold==32);
 reset();envfail=1;start();assert(exits==1&&!switches&&!kills&&!reaps);envfail=0;
 for(identity=1;identity<=4;identity++) {reset();forkresult=24;start();assert(exits==1 && !switches && !kills && !reaps);}
 puts("carrier: 13 production admission/cleanup cases PASS");return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d);(p/"test.c").write_text(mock+src.replace('#define L(p,n) (*(long *)((p)+(n)))','#define L(p,n) (*(int32_t *)((p)+(n)))')+main)
 subprocess.run(["cc","-std=gnu89","-w",str(p/"test.c"),"-o",str(p/"test")],check=True)
 subprocess.run([str(p/"test")],check=True)
if len(sys.argv)>1:
 sys.path.insert(0,str(root/"tools"));import rsrcedit
 data=pathlib.Path(sys.argv[1]).read_bytes()
 assert data[0xe0:0xe8]==b"APPLCOff"
 o,n,_=rsrcedit.resource(data,b"CODE",0)
 assert struct.unpack_from(">IIII",data,o)==(40,256,8,32) and n==24
 o,n,_=rsrcedit.resource(data,b"CODE",1);assert n>100
 print("carrier: native CODE world and creator PASS")
