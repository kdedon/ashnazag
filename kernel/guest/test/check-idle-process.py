#!/usr/bin/env python3
import pathlib,sys,tempfile,subprocess
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('static long\nmac_cpu()');z=s.index('\n#endif',a);f=s[a:z]
assert '(st.pr_utime.tv_nsec + st.pr_stime.tv_nsec) / 1000000' in f and 'found>=32' in f
p=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define O_RDONLY 0
#define PIOCPSINFO 1
#define PIOCSTATUS 2
struct dirent {char *d_name;};typedef int DIR;
typedef struct {long tv_sec,tv_nsec;} timespec_t;
typedef struct {long pr_pid,pr_ppid;int pr_zomb;char pr_fname[16],pr_sname;} prpsinfo_t;
typedef struct {timespec_t pr_utime,pr_stime;int pr_cursig;struct {long sigbits[4];}pr_sigpend,pr_sighold;} prstatus_t;
long macpid=100;int scenario,index_,fdcount,logs;struct dirent ent={"100"};
DIR *opendir(char*n){assert(!strcmp(n,"/proc"));index_=0;return scenario==1?0:(DIR*)1;}
struct dirent *readdir(DIR*d){return index_++<(scenario==5?33:scenario==4?1:4)?&ent:0;}
int closedir(DIR*d){assert(!fdcount);return 0;}
int open(char*n,int m){fdcount++;return index_;}
int close(int fd){fdcount--;return 0;}
int ioctl(int fd,int cmd,void*p){if(cmd==PIOCPSINFO){prpsinfo_t*a=p;memset(a,0,sizeof *a);a->pr_pid=fd==1?100:fd==2?101:fd==3?102:103;a->pr_ppid=fd==2?100:1;if(scenario==5)a->pr_pid=100;if(scenario==4)a->pr_pid=999;a->pr_zomb=fd==4&&scenario!=5;memset(a->pr_fname,'x',16);a->pr_sname='S';return 0;}else{prstatus_t*a=p;if(scenario==2)return -1;memset(a,0,sizeof *a);a->pr_utime.tv_sec=fd==1?2:1;a->pr_stime.tv_sec=3-fd+1;a->pr_utime.tv_nsec=fd==1?500000002:250000;a->pr_stime.tv_nsec=fd==1?600000003:750000;a->pr_cursig=6;a->pr_sigpend.sigbits[0]=0x20;a->pr_sighold.sigbits[0]=0x100;return 0;}}
void t_info(char*n,char*f,long pid,long ppid,char*name,int state,long ut,long sy,int sig,long pending,long held){assert(!strcmp(n,"finder_idle_process")&&strlen(name)==16&&state=='S'&&sig==6&&pending==0x20&&held==0x100);logs++;}
'''
t=r'''
int main(void){assert(mac_cpu()==9101&&logs==2&&!fdcount);scenario=1;assert(mac_cpu()==-1&&!fdcount);scenario=2;assert(mac_cpu()==-1&&!fdcount);scenario=4;assert(mac_cpu()==-1&&!fdcount);scenario=5;logs=0;assert(mac_cpu()==-1&&logs==32&&!fdcount);puts("idle-process: 5 actual procfs filter/CPU/bounds/failure cases PASS");return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+t);o=pathlib.Path(d)/'t';subprocess.run(['cc','-std=gnu89','-fsanitize=undefined','-Werror',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
