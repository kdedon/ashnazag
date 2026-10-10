#!/usr/bin/env python3
import pathlib,sys,tempfile,subprocess
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('long before, after, stats[8]');z=s.index('\n\t}\n\thost("shot " SHOT "idle")',a);f=s[a:z]
p=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
long first,last;int cpu_calls,cpupass,cpufail,readfail;
long mac_cpu(void){return cpu_calls++ ? last:first;}
int getksym(char*n,unsigned long*a,unsigned long*i){assert(!strchr(n,'+'));assert(!strcmp(n,"uin_evstats")||!strcmp(n,"uin_evlast")||!strcmp(n,"uin_evbuttons"));*a=!strcmp(n,"uin_evstats")?1000:2000;return 0;}
int kmemrw(unsigned long a,long*v,int w){assert((a>=1000&&a<1032)||(a>=2000&&a<2032));if(!w)*v=7;return 0;}
int setsym(char*n,long v){return 0;}long t_kmem(char*n){assert(0);return 0;}
long getsym(char*n){assert(!strcmp(n,"uin_evbuttoncount"));return 0;}
int idlewne81(int arm){return 1;}
int idlepm81(int arm){return 1;}
void host(char*s){}void pause_ms(long t){assert(t==120000);}void t_info(char*n,char*f,...){}
int t_check(char*n,int ok,char*f,...){if(!strcmp(n,"finder_idle_cpu_bounded")){if(ok)cpupass++;else cpufail++;}if(!strcmp(n,"finder_idle_cpu_read")&&!ok)readfail++;return ok;}
void run(void){
'''
t=r'''
}
int main(void){first=100;last=110;run();assert(cpupass==1&&cpufail==0);cpu_calls=0;last=24100;run();assert(cpupass==2);cpu_calls=0;last=24101;run();assert(cpufail==1);cpu_calls=0;first=-1;run();assert(readfail==1&&cpufail==1);puts("idle-harness: 4 actual CPU gate cases PASS");return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+t);o=pathlib.Path(d)/'t';subprocess.run(['cc','-std=gnu89','-fsanitize=undefined',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
