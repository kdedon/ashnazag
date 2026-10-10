#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
static int provider, data, listen_on, spawnfail, died, badport, timeout_on;
static int checks, failures, forks, kills, waits, reaps, ready_reads, ready_checked, unlinks;
static long clock_ms;
static char output[24576];
static int mock_access(const char *s,int mode) {
 (void)mode;
 if(strstr(s,"OTProvProbe"))return provider?0:-1;
 if(strstr(s,"OTDataProbe"))return data?0:-1;
 if(strstr(s,"OTListenProbe"))return listen_on?0:-1;
 if(strstr(s,"OTStockProbe"))return -1;
 abort();
}
static int mock_open(const char *s,int flags,...) {
 (void)flags;
 return strstr(s,"otudp.ready")?41:42;
}
static ssize_t mock_read(int fd,void *b,size_t n) {
 if(fd==41) {
  unsigned char *p=b;ready_reads++;
  p[0]=0x1b;p[1]=0x5f;
  if(ready_reads==1)return 1;
  if(badport||ready_reads==2)p[1]=0x60;
  return 2;
 }
 if(timeout_on)return 0;
 if(strlen(output)>=n)abort();
 memcpy(b,output,strlen(output));return strlen(output);
}
static int mock_close(int fd) {(void)fd;return 0;}
static int mock_unlink(const char *s) {if(strstr(s,"otudp.ready"))unlinks++;return 0;}
static pid_t mock_fork(void) {forks++;return spawnfail?-1:1234;}
static int mock_execl(const char *p,const char *a,...) {(void)p;(void)a;abort();}
static void mock_exit(int e) {(void)e;abort();}
static pid_t mock_waitpid(pid_t p,int *status,int flags) {
 if(p!=1234||flags!=WNOHANG)abort();waits++;
 if(died){*status=512;reaps++;return p;}return 0;
}
static int mock_kill(pid_t p,int s) {if(p!=1234||s!=SIGTERM)abort();kills++;return 0;}
static int t_waitchild(pid_t p,int *s,int secs) {
 if(p!=1234||secs!=3||kills!=1)abort();*s=0;reaps++;return p;
}
static long t_now_ms(void) {return clock_ms;}
static void pause_ms(long ms) {clock_ms+=ms;}
static void t_rearm(int s) {if(s!=240&&s!=120)abort();}
static void host(const char *s) {(void)s;}
static int t_check(const char *name,int ok,const char *fmt,...) {
 (void)fmt;checks++;if(!ok)failures++;
 if(strstr(name,"udp_ready"))ready_checked=ready_reads;
 return ok;
}
static int mock_printf(const char *fmt,...) {(void)fmt;return 0;}
#define access mock_access
#define open mock_open
#define read mock_read
#define close mock_close
#define unlink mock_unlink
#define fork mock_fork
#define execl mock_execl
#define _exit mock_exit
#define waitpid mock_waitpid
#define kill mock_kill
#define printf mock_printf
#include "tests/src/ot81-run.h"
#undef printf
static void run(int prov,int dat,int lis,int failspawn,int dead,int wrong,int timed) {
 int i,total=prov?46:34,expected_fail;
 provider=prov;data=dat;listen_on=lis;spawnfail=failspawn;died=dead;badport=wrong;timeout_on=timed;
 checks=failures=forks=kills=waits=reaps=ready_reads=ready_checked=unlinks=0;clock_ms=0;
 total+=(dat?77:0)+(lis?46:0);output[0]=0;
 memset(output,'x',17000);output[17000]=0;strcat(output,"\n");
 for(i=0;i<total;i++)strcat(output,"otprobe P contract\n");
 strcat(output,"otprobe done\n");
 ot81probe("network");
 expected_fail=(dat&&(failspawn||dead||wrong))+(timed?2:0);
 if(failures!=expected_fail)abort();
 if(dat) {
  if(forks!=1||unlinks!=2)abort();
  if(!failspawn&&!dead) {if(kills!=1||reaps!=1)abort();}
  else if(dead) {if(reaps!=1||kills)abort();}
  else if(kills||reaps)abort();
  if(!failspawn&&!dead&&!wrong&&ready_checked!=3)abort();
 }else if(forks||kills||waits||reaps||unlinks)abort();
 if(clock_ms>185000)abort();
}
int main(void) {
 run(0,0,0,0,0,0,0);run(1,0,0,0,0,0,0);
 run(1,1,0,0,0,0,0);run(1,0,1,0,0,0,0);run(1,1,1,0,0,0,0);
 run(1,1,0,1,0,0,0);run(1,1,0,0,1,0,0);run(1,1,0,0,0,1,0);run(1,1,1,0,0,0,1);
 puts("PASS9 production harness cases: flags/counts, partial/wrong readiness, spawn/exit failures, cleanup, oversized output and deadline");return 0;
}
