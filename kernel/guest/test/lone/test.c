#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include "cpmio.h"
#include "guest_lone.h"
typedef char *caddr_t;
#define GUEST_NVEC 256
#define GDF_USER 1
#define GD_EMULATE 2
#define GD_SYSCALL 3
#define GD_REFLECT 1
#define GPF_PRIV 2
#define GPF_FTRAP 256
#define GPF_FRAME68K 0x10000
#define USTKCLEAR 1
#define GPK_OTHER 0
#define GPK_STOP 1
#define GPK_RESET 2
#define GPK_MMU 4
struct modwrapper { int dummy; };
struct guest_disp { int gd_flags, gd_kind; int (*gd_fn)(); };
struct guest_profile { char *gpf_name; struct modwrapper *gpf_wrapper;
	struct guest_disp gpf_disp[256]; int (*gpf_fsig)(); };
struct guest_proc { unsigned gp_flags, gp_vsr, gp_vpend;
	unsigned long gp_vusp, gp_vvbr, gp_vcacr, gp_ualine; int gp_pkind; };
struct frame { long pc, usp; unsigned fv, sr; char padding[100]; };
#define GR_PC(r) (((struct frame *)(r))->pc)
#define GR_FV(r) (((struct frame *)(r))->fv)
#define GR_SR(r) (((struct frame *)(r))->sr)
#define GR_USP(r) (((struct frame *)(r))->usp)
#define G16(b) ((unsigned char)(b)[0]*256+(unsigned char)(b)[1])
#define GUESTP(p) (&model_gp)
static struct guest_proc model_gp;
static struct { int u_sigflag; } u;
static void *curproc;
static long vectors[256];
static int opword, imm, copyfail, reflectfail, privret=1, attachret;
static int signal_seen, tails, reflects, privcalls, setsr, added;
static long rpc; static int rfv, rn; static char *rtail;
int fpu_present;
static int __amix_fsig(void *p) { return 0; }
static long fuword(caddr_t p) { return vectors[(uintptr_t)p/4]; }
#define psignal model_psignal
static void psignal(void *p, int s) { signal_seen=s; }
static void guest_trapret(void) { tails++; }
static int guest_reflect(struct guest_proc *g, char *r,long pc,int fv,char *tail,int n,int level)
	{ reflects++; rpc=pc; rfv=fv; rn=n; rtail=tail; return reflectfail?-1:0; }
static int pkind;
static int guest_privtrap(struct guest_proc *g,char *r,int v) { privcalls++;g->gp_pkind=pkind;return privret; }
static int guest_profile_add(struct guest_profile *p) { added++;return 7; }
static int attached;
static struct guest_profile *owner;
static int guest_attach(struct guest_profile *p) { if (attached) return EBUSY;
	if (attachret) return attachret;
	attached=1; owner=p;return 0; }
static int guest_detach(struct guest_profile *p) { if(!attached||owner!=p)return ENXIO;attached=0;return 0; }
static int removed;
static void guest_profile_del(struct guest_profile *p) { removed++; }
static int outfail;
static int copyout(caddr_t a,caddr_t b,int n) { if(outfail)return 1;memcpy(b,a,n);return 0; }
#define getminor(d) ((d)&255)
struct cred { int unused; };
struct modwrapper cpmguest_wrapper;
#include "production.c"
#include "driver.c"
static int checks;
#define CHECK(x) do { checks++; if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static struct frame r;
static void reset(int v) { memset(&r,0,sizeof r);r.pc=4096;r.fv=v*4;
	memset(&model_gp,0,sizeof model_gp); model_gp.gp_vsr=0x2700;memset(vectors,0,sizeof vectors);
	memset(&guest_lone_stat,0,sizeof guest_lone_stat);u.u_sigflag=0;signal_seen=tails=reflects=privcalls=setsr=0;
	opword=0x1234;imm=0x2000;copyfail=reflectfail=0;fpu_present=0;privret=1;pkind=GPK_OTHER; }
int main(void) {
	int v,f;struct guest_profile pf;struct modwrapper wrapper;
	memset(&pf,0xff,sizeof pf);
	CHECK(guest_lone_init(&pf,"cpm",&wrapper)==7 && added==1);
	CHECK(pf.gpf_wrapper==&wrapper && !strcmp(pf.gpf_name,"cpm"));
	for(v=0;v<256;v++) {
		if(v>=2 && v<48 && !(v>11&&v<32)) {
			CHECK(pf.gpf_disp[v].gd_fn==guest_lone_trap && pf.gpf_disp[v].gd_flags==GDF_USER);
			CHECK(pf.gpf_disp[v].gd_kind==(v==2||v==8||v==11?GD_EMULATE:v==32?GD_SYSCALL:GD_REFLECT));
		} else CHECK(!pf.gpf_disp[v].gd_fn && !pf.gpf_disp[v].gd_flags);
	}
	reset(2);attachret=16;model_gp.gp_vsr=123;
	CHECK(guest_lone_enter(&pf)==16 && model_gp.gp_vsr==123 && !guest_lone_stat.entered);
	attachret=0;CHECK(!guest_lone_enter(&pf));
	CHECK(model_gp.gp_flags==(GPF_PRIV|GPF_FTRAP) && model_gp.gp_vsr==0x2700 && !model_gp.gp_vpend);
	CHECK(!model_gp.gp_vusp && !model_gp.gp_vvbr && !model_gp.gp_vcacr && guest_lone_stat.entered==1);
	for(v=2;v<48;v++) {
		if(v==8||v==11) continue;
		reset(v);
		if(v<10||v==32||(v>11&&v<32)) CHECK(guest_lone_trap(&model_gp,(char*)&r,v)==1 && !tails);
		else {
			CHECK(!guest_lone_trap(&model_gp,(char*)&r,v) && signal_seen==SIGILL && tails==1);
			reset(v);vectors[4]=8192;
			CHECK(!guest_lone_trap(&model_gp,(char*)&r,v) && reflects==1 && rfv==16 && rn==2);
			CHECK(rpc==(v>32?4094:4096) && !signal_seen);
		}
	}
	for(v=3;v<48;v++) {
		if(v==8||v==11||v==32) continue;
		for(f=0;f<=5;f++) {
			reset(v);vectors[v]=8192;r.fv=(f<<12)|(v*4);
			CHECK(guest_lone_trap(&model_gp,(char*)&r,v)==(f==1||f==5));
			if(f==1||f==5) CHECK(!reflects&&!tails);
			else {CHECK(reflects==1&&rpc==4096&&rfv==(int)r.fv&&rn==(f==4?10:f?6:2));
				CHECK(rtail==(char*)&r+72 && tails==1 && u.u_sigflag==(f?USTKCLEAR:0));}
		}
	}
	/* guest_privtrap's answer: 1 the host's, the trap's vector with the CPU frame, another with a format 0 one */
	reset(8);CHECK(guest_lone_trap(&model_gp,(char*)&r,8)==1&&privcalls==1&&!reflects&&!tails);
	reset(8);privret=8;vectors[8]=8192;r.fv=0x2000|32;
	CHECK(!guest_lone_trap(&model_gp,(char*)&r,8)&&reflects==1&&rfv==(int)r.fv&&rn==6);
	reset(8);privret=4;vectors[4]=8192;r.fv=0x2000|32;
	CHECK(!guest_lone_trap(&model_gp,(char*)&r,8)&&rfv==16&&rn==2&&rpc==4096);
	reset(8);privret=11;vectors[11]=8192;
	CHECK(!guest_lone_trap(&model_gp,(char*)&r,8)&&rfv==44&&rn==2);
	reset(11);privret=11;
	CHECK(!guest_lone_trap(&model_gp,(char*)&r,11)&&signal_seen==SIGILL);
	reset(8);privret=0;pkind=GPK_RESET;
	CHECK(!guest_lone_trap(&model_gp,(char*)&r,8)&&!reflects&&guest_lone_stat.resets==1&&guest_lone_stat.privileged==1);
	reset(8);privret=0;pkind=GPK_STOP;
	CHECK(!guest_lone_trap(&model_gp,(char*)&r,8)&&guest_lone_stat.stops==1&&guest_lone_stat.privileged==1);
	reset(11);privret=0;pkind=GPK_MMU;
	CHECK(!guest_lone_trap(&model_gp,(char*)&r,11)&&!guest_lone_stat.privileged&&!reflects);
	reset(10);vectors[10]=8192;reflectfail=1;r.fv|=0x2000;
	CHECK(!guest_lone_trap(&model_gp,(char*)&r,10)&&signal_seen==SIGSEGV&&tails==1&&u.u_sigflag==USTKCLEAR);
	CHECK(cpm_flag==0);
	attached=0;attachret=0;
	CHECK(cpm_load()==7 && cpm_profile.gpf_wrapper==&cpmguest_wrapper);
	{ dev_t dev=1; struct guest_lone_stat stats;
		CHECK(cpm_open(&dev,0,0,0)==ENXIO&&!cpm_nopen);
		dev=0;CHECK(!cpm_open(&dev,0,0,0)&&cpm_nopen==1);
		CHECK(cpm_unload()==EBUSY&&!removed);
		CHECK(cpm_ioctl(dev,CPMIOC_ENTER,0,0,0,0)==0&&attached&&owner==&cpm_profile);
		CHECK(cpm_ioctl(dev,CPMIOC_ENTER,0,0,0,0)==EBUSY);
		CHECK(cpm_ioctl(dev,CPMIOC_STAT,(char*)&stats,0,0,0)==0&&stats.entered==1);
		outfail=1;CHECK(cpm_ioctl(dev,CPMIOC_STAT,(char*)&stats,0,0,0)==EFAULT);outfail=0;
		CHECK(cpm_ioctl(dev,999,0,0,0,0)==EINVAL);
		owner=&pf;CHECK(cpm_ioctl(dev,CPMIOC_LEAVE,0,0,0,0)==ENXIO&&attached);
		owner=&cpm_profile;CHECK(!cpm_ioctl(dev,CPMIOC_LEAVE,0,0,0,0)&&!attached);
		CHECK(cpm_ioctl(dev,CPMIOC_LEAVE,0,0,0,0)==ENXIO);
		CHECK(!cpm_close(dev,0,0,0)&&!cpm_nopen);
		CHECK(!cpm_unload()&&removed==1);
	}
	printf("%d lone-guest and CP/M production checks PASS\n",checks);return 0;
}
