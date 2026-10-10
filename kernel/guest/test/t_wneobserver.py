#!/usr/bin/env python3
"""Check bounded WNE sampling ends before production CPU accounting."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[3]/'tests/src/t_mac.c').read_text()
a=s.index('t_check("finder_idle_wne_armed",idlewne81(1),"")');z=s.index('finder_idle_cpu_bounded',a);part=s[a:z]
assert part.index('pause_ms(1000L)')<part.index('idlewne81(0)')<part.index('before=mac_cpu()')<part.index('pause_ms(120000L)')<part.index('after = mac_cpu()')
assert 'after-before<=24000L' in s
f=s[s.index('static int\nidlewne81('):s.index('static int\npublicwindowtitle81(')]
h=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
int scenario,flag=7,writes,reads,sets;
int setsym(char*n,long v){assert(!strcmp(n,"uin_frontdiag"));sets++;if(scenario==1)return -1;flag=v;return 0;}
int getksym(char*n,unsigned long*a,unsigned long*i){assert(!*a&&!*i);assert(!strcmp(n,"uin_wnestats")||!strcmp(n,"uin_wnelast"));*a=!strcmp(n,"uin_wnestats")?100:200;return scenario==2?-1:0;}
int kmemrw(unsigned long a,long*v,int write){assert((a>=100&&a<116)||(a>=200&&a<224));if(write){assert(!*v);writes++;}else{reads++;*v=3;}return scenario==3?-1:0;}
void t_info(char*n,char*f,...){ }
'''
t=r'''
int main(void){int i,n=0;
for(i=0;i<4;i++){scenario=i;flag=7;sets=writes=reads=0;assert(idlewne81(1)==!i);assert(flag==(i==0?1:i==1?7:0));n++;
scenario=i;flag=1;sets=writes=reads=0;assert(idlewne81(0)==!i);assert(sets==1&&!writes&&flag==(i==1?1:0));n++;}
printf("wneobserver: %d actual admission/lookup/disable cases PASS\n",n);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(h+f+t)
 subprocess.run(['cc','-std=gnu89','-Wall','-Werror','-fsanitize=undefined',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
