#!/usr/bin/env python3
"""Check production-default idle qualification without changing module settings."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3];s=(root/'tests/src/t_mac.c').read_text()
body=s[s.index('static long idledefaultvalues[5];'):s.index('static int\nidlewne81(')]
h=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
static char *pmopt,*stubopt;
char *getenv(const char *s){return !strcmp(s,"MACPMIDLE")?pmopt:stubopt;}
static char *names[]={"uin_pmidlestate","uin_pmidlerequests","uin_pmidleallocations","uin_pmidlerestores","uin_pmidlewatchedcalls","uin_pmidle","uin_pmidlestub"};
static long vals[7]={2,8,8,6,0,1,1};static int writes,fault;
long getsym(char *name){int i;for(i=0;i<7;i++)if(!strcmp(names[i],name))return vals[i];return -1;}
int getksym(char *name,unsigned long *a,unsigned long *info){int i;assert(*a==0);for(i=0;i<7;i++)if(!strcmp(names[i],name)){*a=(unsigned long)(vals+i);return fault?-1:0;}return -1;}
int kmemrw(unsigned long a,long *v,int write){if(fault)return -1;if(write){writes++;*(long *)a=*v;}else *v=*(long *)a;return 0;}
int setsym(char *name,long value){int i;writes++;for(i=0;i<7;i++)if(!strcmp(name,names[i])){vals[i]=value;return 0;}return -1;}
void t_info(char *name,char *fmt,...){ }
static int checks;
#define OK(x) do{assert(x);checks++;}while(0)
'''
t=r'''
int main(void){
 OK(idlepm81(1)&&writes==0);OK(idlepm81(0)&&writes==0);
 vals[1]++;OK(!idlepm81(0)&&writes==0);vals[1]--;
 vals[2]++;OK(!idlepm81(0)&&writes==0);vals[2]--;
 vals[3]++;OK(!idlepm81(0)&&writes==0);
 vals[0]=0;OK(idlestub81(2)&&writes==0);
 vals[3]++;OK(!idlestub81(2)&&writes==0);vals[3]--;
 vals[0]=2;vals[5]=0;OK(!idlepm81(1)&&writes==0);vals[5]=1;
 vals[6]=0;OK(!idlepm81(1)&&writes==0);vals[6]=1;
 fault=1;OK(!idlepm81(1)&&writes==0);fault=0;
 vals[0]=0;OK(!idlepm81(1)&&writes==0);
 vals[0]=2;OK(idlepm81(1)&&writes==0);
 printf("pmdefault: %d actual harness cases PASS\n",checks);return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h+body+t)
 subprocess.run(['cc','-std=gnu89','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
