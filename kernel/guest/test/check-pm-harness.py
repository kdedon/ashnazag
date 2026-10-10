#!/usr/bin/env python3
import pathlib,sys,tempfile,subprocess
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('static int\nidlepm81');z=s.index('\nstatic int\nidlewne81',a);f=s[a:z]
b=s[s.index('long before, after, stats[8]'):s.index('finder_idle_cpu_read',s.index('long before, after, stats[8]'))]
assert b.index('idlepm81(1)')<b.index('before=mac_cpu()')<b.index('pause_ms(120000L)')<b.index('after = mac_cpu()')<b.index('idlepm81(0)')
assert 'after-before<=24000L' in s
p=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
int idlestub81(int phase){return 1;}
int scenario,optmode,armed,disarmed,writes,reads,info_calls;
char *getenv(char*n){if(!strcmp(n,"MACPMSTUB"))return 0;assert(!strcmp(n,"MACPMIDLE"));return optmode==0?0:optmode==1?"1":"0";}
int setsym(char*n,long v){assert(!strcmp(n,"uin_pmidle")||!strcmp(n,"uin_pmidletracecount"));if(!strcmp(n,"uin_pmidle")){if(v)armed++;else disarmed++;}return scenario==1?-1:0;}
int getksym(char*n,unsigned long*a,unsigned long*i){assert(*a==0&&*i==0&&!strchr(n,'+'));*a=!strcmp(n,"uin_watchskipped")?4000:scenario==3?0:!strcmp(n,"uin_pmidletrace")?2000:1000;return scenario==2?-1:0;}
int kmemrw(unsigned long a,long*v,int w){assert(a==4000||a==1000||(a>=2000&&a<2384));if(w){assert(*v==0);writes++;}else{reads++;*v=3;}return scenario==4||(scenario==5&&w)||(scenario==8&&a>=2000)?-1:0;}
long getsym(char*n){assert(!strcmp(n,"uin_pmidletracecount"));return scenario==6?-1:scenario==7?9:scenario>=8?1:0;}
void t_info(char*n,char*f,...){info_calls++;}
'''
t=r'''
int main(void){int i,j,n=0;for(i=0;i<3;i++)for(j=0;j<10;j++){optmode=i;scenario=j;armed=disarmed=writes=reads=info_calls=0;assert(idlepm81(1)==(i==0?1:i==1?(j==0||j>=6):j!=1));assert(disarmed==(i!=0));if(i==1){assert(info_calls==3);assert(armed==(j==0||j>=6));}else assert(!armed&&!reads&&!writes&&!info_calls);n++;armed=disarmed=writes=reads=info_calls=0;assert(idlepm81(0)==(i==0?1:i==1?(j==0||j==5||j==9):j!=1));assert(disarmed==(i!=0)&&!armed&&!writes);n++;}printf("pm-harness: %d actual opt-in/lookup/rollback cases PASS\n",n);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+t);o=pathlib.Path(d)/'t';subprocess.run(['cc','-std=gnu89','-fsanitize=undefined','-Werror',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
