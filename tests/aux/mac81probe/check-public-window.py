#!/usr/bin/env python3
import pathlib,sys,subprocess,tempfile
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('static int\npublicwindowtitle81');z=s.index('\nstatic int\nmonitorstate81',a);f=s[a:z]
p=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#define O_RDONLY 0
#define SEEK_SET 0
struct pollfd;
long clockms,macpid=42;int scenario;
long t_now_ms(void){return clockms;}
long getsym(char*n){if(!strcmp(n,"uin_frontcount"))return scenario==1?5:6;if(!strcmp(n,"uin_frontpid"))return scenario==2?43:42;return scenario==3?1:2;}
int getksym(char*n,unsigned long*a,unsigned long*i){assert(!strcmp(n,"uin_fronttitle"));*a=1000;return scenario==4?-1:0;}
int open(char*n,int m){assert(!strcmp(n,"/dev/kmem"));return 7;}
off_t lseek(int fd,off_t a,int w){assert(fd==7&&a==1000&&w==0);return a;}
int read(int fd,char*b,int n){assert(fd==7&&n==256);memset(b,0,n);strcpy(b,scenario==5?"Desktop":"Appearance");if(scenario==6)b[255]=1;return scenario==7?255:256;}
int close(int fd){return 0;}
int poll(struct pollfd*p,long n,int ms){clockms+=ms;return 0;}
void t_info(char*n,char*f,char*v){}
'''
t=r'''
int main(void){int i;for(i=0;i<8;i++){scenario=i;clockms=0;assert(publicwindowtitle81(0,"Appearance",5)==(i==0));}puts("public-window: 8 actual reader cases PASS");return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+t);o=pathlib.Path(d)/'t';subprocess.run(['cc','-std=gnu89','-fsanitize=undefined',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
