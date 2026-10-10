#!/usr/bin/env python3
import pathlib,subprocess,sys,tempfile
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('static int\nidlewne81');z=s.index('\nstatic int\npublicwindowtitle81',a);f=s[a:z]
assert f.index('base[i]=0;info=0;')<f.index('getksym(names[i],')
idle=s[s.index('long before, after, stats[8]'):s.index('finder_idle_cpu_read',s.index('long before, after, stats[8]'))]
assert idle.index('idlewne81(1)')<idle.index('before=mac_cpu()')<idle.index('pause_ms(120000L)')<idle.index('idlewne81(0)')
p=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
int scenario,writes,reads,armed,disarmed,infos;
int setsym(char*n,long v){assert(!strcmp(n,"uin_frontdiag"));if(v)armed++;else disarmed++;return scenario==6?-1:0;}
int getksym(char*n,unsigned long*a,unsigned long*i){assert(*a==0&&*i==0);assert(!strchr(n,'+'));assert(!strcmp(n,"uin_wnestats")||!strcmp(n,"uin_wnelast"));*a=!strcmp(n,"uin_wnestats")?1000:2000;if(scenario==2)*a=0;return scenario==1?-1:0;}
int kmemrw(unsigned long a,long*v,int w){assert((a>=1000&&a<1016)||(a>=2000&&a<2024));if(w){assert(*v==0);writes++;}else{reads++;*v=7;}return scenario==3 || (scenario==4&&!w) || (scenario==5&&w)?-1:0;}
void t_info(char*n,char*f,...){infos++;}
'''
t=r'''
int main(void){int i;for(i=0;i<7;i++){scenario=i;writes=reads=armed=disarmed=infos=0;assert(idlewne81(1)==(i==0||i==4));assert(!reads&&(!armed||writes==10));writes=reads=armed=disarmed=infos=0;assert(idlewne81(0)==(i==0||i==5));assert(!writes&&!armed&&disarmed==1);assert(infos==((i==0||i==5)?2:0));}puts("idle-wne-harness: 14 actual reset/read failure cases PASS");return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+t);o=pathlib.Path(d)/'t';subprocess.run(['cc','-std=gnu89','-fsanitize=undefined','-Werror',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
