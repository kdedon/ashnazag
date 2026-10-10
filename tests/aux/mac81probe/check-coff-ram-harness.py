#!/usr/bin/env python3
"""Exercise the maintained COFF reader's session and mapped-span checks."""
import pathlib,sys,subprocess,tempfile,os
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('static int\ncofframrange(');b=s.index('\nstatic void\ncoffcommtarget(',a);fn=s[a:b]
pre=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef struct {int pr_pid,pr_ppid,pr_zomb;char pr_fname[20];} prpsinfo_t;
typedef struct {void *pr_vaddr;unsigned long pr_size;long pr_mflags;} prmap_t;
struct shmid_ds {int shm_cpid;unsigned long shm_segsz;};
#define TLOW 0x744c4f57
#define IPC_STAT 1
#define PIOCPSINFO 2
#define PIOCNMAP 3
#define PIOCMAP 4
#define MA_READ 4
#define MA_SHARED 8
static int macpid=77,fail,count=1,ioctls;
static prpsinfo_t ps;static struct shmid_ds ram;static prmap_t maps[2];
int shmget(int key,int size,int flags){assert(key==TLOW&&!size&&!flags);return fail==5?-1:2;}
int shmctl(int id,int cmd,struct shmid_ds *p){assert(id==2&&cmd==IPC_STAT);if(fail==6)return -1;*p=ram;return 0;}
int ioctl(int fd,int cmd,void *p){assert(fd==9);ioctls++;if(fail==cmd)return -1;switch(cmd){case PIOCPSINFO:*(prpsinfo_t*)p=ps;break;case PIOCNMAP:*(int*)p=count;break;case PIOCMAP:memcpy(p,maps,sizeof maps);break;default:assert(0);}return 0;}
'''
test=r'''
int main(void){int checks=0,i;unsigned long begin;
ps.pr_pid=77;ps.pr_ppid=12;strcpy(ps.pr_fname,"startmac");ram.shm_cpid=77;ram.shm_segsz=36UL*1024*1024;
maps[0].pr_size=32UL*1024*1024;maps[0].pr_mflags=MA_READ|MA_SHARED;
#define OK(x) do{assert(x);checks++;}while(0)
OK(cofframrange(9,0x10000,24));OK(cofframrange(9,0x1c34210,548864));OK(cofframrange(9,0x1fd4070,16));
OK(cofframrange(9,32UL*1024*1024-1,1));OK(!cofframrange(9,32UL*1024*1024-1,2));
OK(!cofframrange(9,0xfffffff0UL,32));OK(!cofframrange(9,0x10000,0));OK(!cofframrange(9,36UL*1024*1024,1));
ps.pr_pid=88;ps.pr_ppid=77;strcpy(ps.pr_fname,"CommandShell");OK(cofframrange(9,0x1c34210,548864));
ps.pr_ppid=12;OK(!cofframrange(9,0x10000,1));ps.pr_ppid=77;strcpy(ps.pr_fname,"foreign");OK(!cofframrange(9,0x10000,1));strcpy(ps.pr_fname,"CommandShell");
ps.pr_zomb=1;OK(!cofframrange(9,0x10000,1));ps.pr_zomb=0;ram.shm_cpid=12;OK(!cofframrange(9,0x10000,1));ram.shm_cpid=77;
for(i=2;i<=6;i++){fail=i;OK(!cofframrange(9,0x10000,1));}fail=0;
count=0;OK(!cofframrange(9,0x10000,1));count=129;OK(!cofframrange(9,0x10000,1));count=1;
maps[0].pr_mflags=MA_READ;OK(!cofframrange(9,0x10000,1));maps[0].pr_mflags=MA_SHARED;OK(!cofframrange(9,0x10000,1));maps[0].pr_mflags=MA_READ|MA_SHARED;
maps[0].pr_vaddr=(void*)0x10000;OK(!cofframrange(9,0xffff,2));maps[0].pr_size=16UL*1024*1024-0x10000;
OK(cofframrange(9,0x10000,24));OK(!cofframrange(9,0x1c34210,548864));
count=2;maps[1].pr_vaddr=(void*)0x1c00000;maps[1].pr_size=0x100000;maps[1].pr_mflags=MA_READ|MA_SHARED;
OK(cofframrange(9,0x1c34210,548864));OK(!cofframrange(9,0x1cfffff,2));
printf("coff RAM harness: %d mapped/session checks PASS\n",checks);return 0;}
'''
# Rendering qualification retains its actual glyph and pixel requirements.
assert 'ok=rows>=4 && rows<=24 && pixels>=128;' in s
assert 'native<=0x1000000L' not in s
with tempfile.TemporaryDirectory() as tmp:
 d=pathlib.Path(tmp);(d/'check.c').write_text(pre+fn+test)
 subprocess.run(['cc','-std=gnu89','-fsanitize=address,undefined','-fno-pie','-no-pie',str(d/'check.c'),'-o',str(d/'check')],check=True)
 subprocess.run([str(d/'check')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
