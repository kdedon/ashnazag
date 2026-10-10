#!/usr/bin/env python3
import pathlib,sys,tempfile,subprocess
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('static long idledefaultvalues[5];');z=s.index('static int\nidlepm81',a);f=s[a:z]
assert 'after-before<=24000L' in s and 'finder_idle_stub_restored' in s
p=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
int marker,wait,scenario,state,requests,allocations,restores,flag,reads,writes,watched;
char *names[]={"uin_pmidlestate","uin_pmidlerequests","uin_pmidleallocations","uin_pmidlerestores","uin_pmidlewatchedcalls"};
char *getenv(char*n){return !strcmp(n,"MACPMSTUB")?(marker==1?"1":marker==2?"0":0):(wait?"1":0);}
long getsym(char*n){assert(!strcmp(n,"uin_pmidle")||!strcmp(n,"uin_pmidlestub"));return 1;}
int setsym(char*n,long v){assert(!strcmp(n,"uin_pmidlestub"));flag=v;return scenario==1?-1:0;}
int getksym(char*n,unsigned long*a,unsigned long*i){int j;assert(!*a&&!*i);for(j=0;j<5;j++)if(!strcmp(n,names[j]))break;assert(j<5);*a=scenario==2?0:100+j;return scenario==3?-1:0;}
int kmemrw(unsigned long a,long*v,int w){int *p=a==100?&state:a==101?&requests:a==102?&allocations:a==103?&restores:&watched;assert(a>=100&&a<=104);if(w){assert(a!=100&&*v==0);writes++;*p=*v;}else{reads++;*v=*p;}return scenario==4||scenario==5&&w?-1:0;}
void t_info(char*n,char*f,...){assert(!strcmp(n,"finder_idle_stub"));}
'''
t=r'''
int main(void){int n=0,i,j;for(i=0;i<3;i++){marker=i;wait=1;state=0;scenario=0;flag=7;reads=writes=0;assert(idlestub81(1)&&flag==(i==1));if(i!=1)assert(!reads&&!writes);n++;}
marker=wait=1;for(j=0;j<=5;j++){scenario=j;state=0;assert(idlestub81(1)==!j);n++;}scenario=0;state=2;requests=7;writes=0;assert(!idlestub81(1)&&requests==7&&!writes);n++;state=0;wait=0;assert(!idlestub81(1)&&!flag);n++;wait=1;state=2;requests=allocations=1;restores=0;assert(idlestub81(0));n++;
state=0;assert(!idlestub81(0));n++;state=2;requests=2;assert(!idlestub81(0));n++;requests=1;allocations=0;assert(!idlestub81(0));n++;allocations=1;restores=1;assert(!idlestub81(0));n++;
state=0;assert(idlestub81(2)&&!flag);n++;state=2;assert(!idlestub81(2)&&!flag);n++;state=0;restores=0;assert(!idlestub81(2)&&!flag);n++;
marker=wait=0;scenario=0;state=2;requests=allocations=3;restores=4;writes=0;flag=7;
assert(idlestub81(1)&&flag==7&&!writes);n++;assert(idlestub81(0)&&!writes);n++;
state=0;restores=5;assert(idlestub81(2)&&flag==7&&!writes);n++;
restores=6;assert(!idlestub81(2)&&!writes);n++;state=2;restores=4;scenario=3;assert(!idlestub81(1)&&!writes);n++;
printf("pm-stub-harness: %d actual marker/publication/restore/failure cases PASS\n",n);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+t);o=pathlib.Path(d)/'t';subprocess.run(['cc','-std=gnu89','-fsanitize=undefined','-Werror',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
