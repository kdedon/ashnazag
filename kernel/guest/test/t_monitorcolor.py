from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'tests/src/t_mac.c').read_text()
s=s[s.index('static int\nmonitorstate81('):s.index('static void\npanel81(')]
header=r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#define SHOT ""
static unsigned char mem[65536];
static int faults,reads,front=1,clicks,badcount,badgray,badflags,geometry;
static long gray,count,clockms;
static long g32(unsigned char *p) {return (long)((uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]);}
static void put(long a,uint32_t v){mem[a]=v>>24;mem[a+1]=v>>16;mem[a+2]=v>>8;mem[a+3]=v;}
static int rdmac(int fd,long a,char *p,int n){(void)fd;reads++;if(reads==faults)return 0;assert(a>=0 && a+n<=65536);memcpy(p,mem+a,n);return 1;}
static void t_info(char *name,char *fmt,...){(void)name;(void)fmt;}
static int passed,failed;
static int t_check(char *name,int ok,char *fmt,...){(void)name;(void)fmt;if(ok)passed++;else failed++;return ok;}
static int windowtitle81(int fd,char *s){(void)fd;assert(!strcmp(s,"Monitors"));return front;}
static long getsym(char *s){return !strcmp(s,"uinter_video_gray")?gray:count;}
static int moveto(int x,int y){assert(x==133 && y==(clicks?88:104));return 1;}
static void host(char *s){if(!strcmp(s,"button 0")){int i=clicks++;if(!badcount)count++;if(!badgray)gray=i;if(!badflags)mem[0x3015]=(mem[0x3015]&~1)|!i;if(geometry)mem[0x302a]++;}}
static long t_now_ms(void){return clockms;}
static void pause_ms(long n){clockms+=n;}
'''
tests=r'''static void reset(void){memset(mem,0,sizeof mem);put(0x8a4,0x2000);put(0x2000,0x3000);put(0x3016,0x4000);put(0x4000,0x5000);mem[0x5021]=8;put(0x9d6,0x6000);mem[0x606e]=1;mem[0x6008]=255;mem[0x6009]=194;mem[0x600a]=255;mem[0x600b]=156;mem[0x3000]=255;mem[0x3001]=205;mem[0x3026]=2;mem[0x3027]=88;mem[0x3028]=3;mem[0x3029]=32;put(0x6076,0x7000);put(0x7000,0x8000);mem[0x8001]=10;mem[0x8003]=61;mem[0x8005]=101;mem[0x8006]=1;mem[0x8007]=68;mem[0x8008]=1;mem[0x8009]=74;mem[0x6011]=1;mem[0x6013]=89;
put(0x608c,0x9000);put(0x9000,0xa000);put(0x9100,0xa100);put(0xa000,0x9100);put(0xa004,0x6000);put(0xa104,0x6000);
mem[0xa009]=36;mem[0xa00b]=95;mem[0xa00d]=53;mem[0xa00f]=148;mem[0xa010]=1;mem[0xa028]=7;memcpy(mem+0xa029,"Colors:",7);
mem[0xa109]=20;mem[0xa10b]=95;mem[0xa10d]=37;mem[0xa10f]=148;mem[0xa110]=1;mem[0xa128]=6;memcpy(mem+0xa129,"Grays:",6);put(0x302a,128);gray=0;count=0;clockms=0;faults=reads=clicks=passed=failed=badcount=badgray=badflags=geometry=0;front=1;}
int main(void){unsigned char b[48];int i,n=0;
reset();assert(monitorstate81(0,b)&&b[47]==8);n++;
for(i=1;i<=5;i++){reset();faults=i;memset(b,0xa5,sizeof b);assert(!monitorstate81(0,b)&&b[0]==0xa5);n++;}
for(i=0;i<5;i++){reset();put(i==0?0x8a4:i==1?0x2000:i==2?0x3016:i==3?0x4000:0x9d6,1);if(i<4){assert(!monitorstate81(0,b));n++;}}
reset();monitorcolor81(0);assert(clicks==2 && failed==0 && count==2 && gray==1);n++;
reset();mem[0x5021]=16;monitorcolor81(0);assert(!clicks && failed==1);n++;
reset();put(0x7000,0);monitorcolor81(0);assert(!clicks && failed==1);n++;
reset();front=0;monitorcolor81(0);assert(!clicks && failed==1);n++;
for(i=0;i<6;i++){reset();if(i==0)put(0xa004,0x6100);if(i==1)mem[0xa010]=0;if(i==2)mem[0xa011]=255;if(i==3)mem[0xa009]=255;if(i==4)mem[0xa029]='X';if(i==5)put(0xa000,0x9000);monitorcolor81(0);assert(!clicks && failed==1);n++;}
reset();badcount=1;monitorcolor81(0);assert(clicks==1 && failed==2 && clockms==10000);n++;
reset();badgray=1;monitorcolor81(0);assert(clicks==2 && failed==2 && clockms==10000);n++;
reset();badflags=1;monitorcolor81(0);assert(clicks==1 && failed==2 && clockms==10000);n++;
reset();geometry=1;monitorcolor81(0);assert(clicks==2 && failed==2);n++;
printf("Monitors switch: %d checks passed\n",n);return 0;}'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(header+s+tests)
 subprocess.run(['cc','-std=gnu89','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
