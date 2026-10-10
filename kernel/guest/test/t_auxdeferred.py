#!/usr/bin/env python3
"""Exercise the production BSD tick frame and its native drain gate."""
import os, pathlib, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[1]
out = pathlib.Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
s = (root / 'mod/auxcore/auxsig.c').read_text()
def body(name):
    start = s.rfind('\nstatic int\n', 0, s.index(name + '(')) if name == 'bsdframe' else s.index(('\nstatic void\n' if name == 'aux_deferrecord' else '\nint\n') + name + '(')
    end = s.index('\n}', s.index(name + '(')) + 2
    return s[start:end]
c = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define BSDFR 64
#define ABIT(s) (1UL << ((s)-1))
#define COMPAT_BSDSIGNALS 4
#define SIGIOT 6
#define APM_TASK 1
#define XSIGS 0
#define USTKCLEAR 1
#define sigmask(s) ABIT(s)
struct aux_proc {unsigned long ap_sv_onstack; long ap_ss_sp; int ap_ss_onstack; long ap_ss_isp; int ap_mac,ap_compat;};
struct guest_proc {struct aux_proc ap; unsigned short gp_vsr;};
#define AUXP(gp) (&(gp)->ap)
struct proc {long p_pid;};
static struct proc proc={23};
struct {struct proc *u_procp; void *u_ar0; unsigned long u_sigoldmask; int u_sigflag;} u;
static long regs[20];
#define GR_USP(r) regs[16]
#define GR_PC(r) regs[17]
#define GR_SR(r) regs[18]
#define GR_VEC(r) regs[19]
#define GR_D(r,n) regs[n]
#define GR_A(r,n) regs[8+(n)]
typedef char *caddr_t;
typedef unsigned long u_long;
static unsigned char frame[64]; static long frameaddr;
static int failcopy,ticks,flushes,failin,tracelines;
static unsigned char nativequeue[10];
static int aux_defertrace;
static long aux_deferticks,aux_deferqueued,aux_deferprints,aux_deferfirst[11],aux_deferlast[11];
static int traceprintf(const char *fmt,...){if(strstr(fmt,"%l"))exit(2);tracelines++;return 0;}
#define printf traceprintf
static void p32(char *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void p16(char *p,uint16_t v){p[0]=v>>8;p[1]=v;}
#define P32 p32
#define P16 p16
static uint32_t g32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int copyout(void *b,caddr_t a,int n){if(failcopy)return 1;memcpy(frame,b,n);frameaddr=(long)a;return 0;}
static int copyin(caddr_t a,caddr_t b,int n){if(failin)return 1;if((long)a!=0xd92||n!=10)exit(2);memcpy(b,nativequeue,n);return 0;}
#define G32(p) g32((const unsigned char *)(p))
#define G16(p) ((unsigned)((const unsigned char *)(p))[0]<<8|((const unsigned char *)(p))[1])
static void dlm_cacheflush(void){flushes++;}
static long aux_mask_out(u_long m){return m;}
static int aux_sig_out(int sig){return sig;}
static void tick(struct guest_proc *gp){(void)gp;ticks++;}
static void (*aux_uitick)(struct guest_proc *)=tick;
static long aux_nsig[32],aux_xsig,aux_xvec,aux_xpc;
static int checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL line%d\n",__LINE__);exit(1);}}while(0)
'''
c += body('bsdframe') + '\n' + body('aux_deferrecord') + '\n' + body('aux_sendsig')
c += r'''
int main(void){
 struct guest_proc gp;int sig,mac,ipl,blocked;long before[20];
 u.u_procp=&proc;u.u_ar0=regs;
 for(sig=1;sig<32;sig++)for(mac=0;mac<8;mac++)for(ipl=0;ipl<8;ipl++)for(blocked=0;blocked<2;blocked++){
  memset(&gp,0,sizeof gp);gp.ap.ap_compat=4;gp.ap.ap_mac=mac;gp.gp_vsr=ipl<<8;
  memset(regs,0,sizeof regs);regs[16]=0x10000;regs[17]=0xabc0;regs[18]=0xc003;regs[0]=123;regs[8]=456;
  u.u_sigoldmask=blocked?0x20:0;ticks=flushes=failcopy=u.u_sigflag=0;
  CHECK(aux_sendsig(&gp,sig,0,(int(*)())0x1234)==1);
  CHECK(frameaddr==0x10000-64 && regs[16]==frameaddr && regs[17]==0x1234);
  CHECK(g32(frame+4)==(unsigned)sig && g32(frame+12)==frameaddr+44);
  CHECK(frame[17]==(sig==6 && (mac&1) && !ipl));
  CHECK(frame[16]==0 && frame[18]==0 && frame[19]==0);
  CHECK(g32(frame+48)==u.u_sigoldmask);
  CHECK((g32(frame+48)==0 && frame[17]==1)==(sig==6 && (mac&1) && !ipl && !blocked));
  CHECK(g32(frame+56)==0xabc0 && g32(frame+60)==0xc003 && regs[18]==3);
  CHECK(g32(frame+20)==123 && g32(frame+28)==456 && flushes==1);
  CHECK(ticks==(sig==6 && mac));
 }
 memset(&gp,0,sizeof gp);gp.ap.ap_compat=4;gp.ap.ap_mac=1;gp.ap.ap_sv_onstack=ABIT(6);gp.ap.ap_ss_sp=0x30000;
 regs[16]=0x10000;regs[17]=0xabc0;regs[18]=3;u.u_sigoldmask=0;failcopy=0;
 CHECK(aux_sendsig(&gp,6,0,(int(*)())0x1234)==1);CHECK(frameaddr==0x30000-64 && gp.ap.ap_ss_onstack==1 && gp.ap.ap_ss_isp==0x10000);
 gp.ap.ap_ss_onstack=0;regs[16]=0x10000;regs[17]=0xabc0;memcpy(before,regs,sizeof regs);failcopy=1;
 CHECK(aux_sendsig(&gp,6,0,(int(*)())0x1234)==0);CHECK(!gp.ap.ap_ss_onstack && !memcmp(before,regs,sizeof regs));
 aux_defertrace=1;failcopy=0;gp.ap.ap_mac=1;gp.ap.ap_compat=4;gp.gp_vsr=0;u.u_sigoldmask=0;
 p32((char*)nativequeue+2,0x123456);p32((char*)nativequeue+6,0x123456);
 for(sig=0;sig<100;sig++)aux_deferrecord(&gp,6,(int(*)())0x1234);
 CHECK(aux_deferticks==100 && aux_deferqueued==100 && aux_deferprints==36 && tracelines==72);
 CHECK(aux_deferfirst[0]==23 && aux_deferfirst[4]==1 && aux_deferfirst[7]==0x123456);
 gp.ap.ap_mac=4;gp.gp_vsr=0x700;u.u_sigoldmask=32;failin=1;
 aux_deferrecord(&gp,6,(int(*)())0x5678);
 CHECK(aux_deferlast[1]==4 && aux_deferlast[3]==0x700 && aux_deferlast[4]==0);
 CHECK(aux_deferlast[5]==32 && aux_deferlast[7]==-1 && aux_deferlast[9]==0x5678);
 CHECK(aux_deferqueued==100 && aux_deferticks==101 && tracelines==72);
 aux_defertrace=0;aux_deferrecord(&gp,6,0);CHECK(aux_deferticks==101);
 aux_defertrace=1;aux_deferrecord(&gp,7,0);gp.ap.ap_mac=0;aux_deferrecord(&gp,6,0);CHECK(aux_deferticks==102 && aux_deferlast[1]==0 && aux_deferlast[4]==0);
 fprintf(stdout,"PASS %d production signal/deferred-frame checks\n",checks);return 0;
}
'''
(out / 'auxdeferred.c').write_text(c)
subprocess.run(['cc','-std=gnu89','-O','-Wall','-Wextra','-Wno-unused-parameter','-Wno-pointer-sign','-Wno-unused-variable','-fsanitize=address,undefined','-no-pie','-o',str(out/'auxdeferred'),str(out/'auxdeferred.c')],check=True)
subprocess.run([str(out/'auxdeferred')],check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
