#!/usr/bin/env python3
"""Exercise the live redirect matcher against changed and unreadable code."""
import os
import sys
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / 'mod/uinter/uipatch.c'
header = r'''
#include <stdint.h>
extern int printf(const char *, ...);
#include <string.h>
#include <assert.h>
typedef char *caddr_t;
struct guest_proc { struct proc *gp_proc; uint32_t gp_flags; unsigned short gp_vsr; };
#define GPF_AWATCH 0x4000
#define GPF_STUB 0x80000000UL
#define GPF_PROFILE 0xfffe0000UL
#define GR_USP(r) (*(uint32_t *)((r) + 0))
#define GR_D(r,n) (*(uint32_t *)((r) + 4 + 4*(n)))
#define GR_A(r,n) (*(uint32_t *)((r) + 36 + 4*(n)))
#define GR_SR(r) (*(unsigned short *)((r) + 64))
#define GR_PC(r) (*(uint32_t *)((r) + 66))
#define G8(p) ((unsigned long)((unsigned char *)(p))[0])
#define G16(p) (G8(p)<<8 | G8((char *)(p)+1))
#define G32(p) (G16(p)<<16 | G16((char *)(p)+2))
#define P8(p,v) (((unsigned char *)(p))[0]=(v)&255)
#define P16(p,v) (P8(p,(v)>>8),P8((char *)(p)+1,v))
#define P32(p,v) (P16(p,(v)>>16),P16((char *)(p)+2,v))
#define SIGSEGV 11
#define SIGKILL 9
#define RLIMIT_STACK 3
#define EACCES 13
#define EINVAL 22
#define EFAULT 14
#define UI_NTASK 2
struct uitask { struct guest_proc *t_gp; long t_forceentry; void *t_proc; int t_force; };
#define LS_INUSE 2
static struct { struct guest_proc *l_active, *l_gp; struct proc *l_proc; int l_state,l_tid; struct uitask l_task[2]; caddr_t l_romaddr; } ui;
static int inputs;
void ui_pmidlewait(struct guest_proc *gp) { (void)gp; }
int ui_inputpoll(struct guest_proc *gp) { (void)gp; inputs++; return 0; }
void ui_forcepollwatch(struct guest_proc *gp,int on) { assert(gp==ui.l_active); if(on) ui.l_task[0].t_force|=4; else ui.l_task[0].t_force &= ~4; }
int ui_forcepollowned(struct guest_proc *gp) { return gp==ui.l_active && (ui.l_task[0].t_force & 4); }
static unsigned char memory[0x100000];
static int writes, deny;
static uintptr_t denyoutaddr, denyinaddr;
static int denyoutnth, outseen, partialout;
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
struct proc { void *p_as; int p_pid; caddr_t p_stkbase; int p_stksize, p_cursig; unsigned long p_hold, p_sig; };
static struct proc proc;
static int signals;
void psignal(struct proc *p,int sig) { assert(p==&proc && sig==SIGSEGV); signals++; }
static struct { struct proc *u_procp; struct { unsigned long rlim_cur; } u_rlimit[4]; } u={&proc};
static int denyprot;
int as_checkprot(void *as,caddr_t addr,unsigned long n,int prot) {
    (void)as; (void)addr; (void)n; assert(prot==7); return denyprot;
}
#define HZ 100
static long lbolt;
#define APM_COFF 4
static struct { int ap_mac; } testaux;
#define AUXP(gp) (&testaux)
struct proc *prfind(int pid) {return pid==proc.p_pid?&proc:0;}
struct uitask *ui_task(struct guest_proc *gp) {return gp==ui.l_gp?&ui.l_task[0]:0;}
int ui_patchshared(struct guest_proc *gp) { (void)gp; return 0; }
#define GUESTP(p) ((struct guest_proc *)0)
int ui_coffworld(struct guest_proc *gp,char *b) { (void)gp;(void)b;return 0; }
int uinter_trace;
long uin_forcewne;
static int forcequeue;
int ui_forceget(void) { return forcequeue; }
void ui_forceconsume(void) { forcequeue=0; }
void ui_forcereserve(long a) { (void)a; }
void ui_forceretry(void) { forcequeue=1; }
void ui_forceenable(int on) { if(!on) forcequeue=0; }
void ui_forcestub(struct guest_proc *gp) { gp->gp_flags |= GPF_STUB; }
void ui_forceend(struct guest_proc *gp) { gp->gp_flags &= ~GPF_STUB; }
int copyin(caddr_t src,caddr_t dst,int n) {
    uintptr_t a=(uintptr_t)src;
    if (deny || (denyinaddr && a==denyinaddr) || a>sizeof memory || n>sizeof memory-a) return 1;
    memcpy(dst,memory+a,n); return 0;
}
int copyout(caddr_t src,caddr_t dst,int n) {
    uintptr_t a=(uintptr_t)dst;
    if (deny || a>sizeof memory || n>sizeof memory-a) return 1;
    if (a==denyoutaddr && denyoutaddr && (!denyoutnth || ++outseen==denyoutnth)) {
        if(partialout) memcpy(memory+a,src,4);
        return 1;
    }
    writes++; memcpy(memory+a,src,n); return 0;
}
int ui_macrange(struct guest_proc *gp,unsigned long a,unsigned long n) {
    return gp && gp->gp_proc==u.u_procp && ui.l_state==LS_INUSE &&
        ui.l_proc==u.u_procp && ui_task(gp) && a>=0x3000UL && !(a&1) && n &&
        a<sizeof memory && n<=sizeof memory-a;
}
void dlm_cacheflush(void) {}
'''
tests = r'''
static void setup(struct guest_proc *gp,char *r,int i) {
    memset(memory,0,sizeof memory); memset(r,0,80);
    gp->gp_flags=GPF_AWATCH | (GPF_DONE(NPATCH)-GPF_DONE(0));
    gp->gp_flags &= ~GPF_DONE(i); gp->gp_vsr=0x700;
    GR_PC(r)=0x10000+uipatches[i].p_at; GR_USP(r)=0x80000;
    P16(memory+0x15a,0x0810); P32(memory+0x400+4*0xac,0x1234);
    memcpy(memory+0x10000,uipatches[i].p_code,uipatches[i].p_len);
    memcpy(memory+0x10000+HFSWALK,hfswalk,sizeof hfswalk);
    P32(memory+0x358,0x2000);
    writes=deny=forcequeue=0; uinter_patched=0; uinter_patches=1;
}
int main(void) {
    struct guest_proc gp;
    union { uint32_t align; char r[80]; } regs;
    char *r=regs.r;
    int i,j,checks=0;
    for(i=0;i<NPATCH;i++) {
        if(!uipatches[i].p_code) continue;
        for(j=0;j<uipatches[i].p_len;j++) {
            setup(&gp,r,i); memory[0x10000+j]^=1;
            if(ui_alwatch(&gp,r)!=0) { printf("unexpected %d:%d pc%x\n",i,j,GR_PC(r)); return 1; }
            assert(GR_PC(r)==0x10000+uipatches[i].p_at);
            assert(!writes && !uinter_patched); checks++;
        }
        setup(&gp,r,i); deny=1;
        assert(!ui_alwatch(&gp,r) && !writes && !uinter_patched); checks++;
    }
    setup(&gp,r,0);
    assert(ui_alwatch(&gp,r)==0 && GR_PC(r)==0x10012);
    assert(writes==1 && uinter_patched==1);
    assert(memory[0x10000+HFSWALK+15]==0x0e);
    assert(memory[0x10000+HFSWALK+23]==0x06); checks++;
    for(j=0;j<sizeof hfswalk;j++) {
        if(j!=15 && j!=23) assert(memory[0x10000+HFSWALK+j]==hfswalk[j]);
    }
    for(j=0;j<sizeof hfswalk;j++) {
        setup(&gp,r,0); memory[0x10000+HFSWALK+j]^=1;
        assert(!ui_alwatch(&gp,r) && !writes && !uinter_patched);
        assert(!(gp.gp_flags & GPF_DONE(0)) && GR_PC(r)==0x10012); checks++;
    }
    setup(&gp,r,0); assert(!ui_alwatch(&gp,r));
    assert(writes==1 && (gp.gp_flags & GPF_DONE(0)));
    GR_PC(r)=0x10012;
    assert(ui_alwatch(&gp,r)==0 && GR_PC(r)==0x10012); checks++;
    setup(&gp,r,0); P16(memory+0x2000+0x4c,0x17);
    assert(ui_alwatch(&gp,r)==1 && GR_PC(r)==0x10020);
    assert(!writes && uinter_patched==1 && (gp.gp_flags & GPF_DONE(0))); checks++;
    assert(!ui_alwatch(&gp,r) && !writes); checks++;
    setup(&gp,r,0); P16(memory+0x2000+0x4c,0x17);
    P32(memory+0x2000,0x2100); P16(memory+0x2100+0x4c,0x17);
    assert(ui_alwatch(&gp,r)==1 && !writes); checks++;
    setup(&gp,r,0); P16(memory+0x2000+0x4c,0x17);
    P32(memory+0x2000,0x2100);
    assert(!ui_alwatch(&gp,r) && writes==1 && GR_PC(r)==0x10012); checks++;
    for(i=0;i<4;i++) {
        setup(&gp,r,0); P16(memory+0x2000+0x4c,0x17);
        if(i==0) P32(memory+0x2000,0x2000);
        if(i==1) P32(memory+0x358,0);
        if(i==2) P32(memory+0x358,0x2001);
        if(i==3) P32(memory+0x2000,0xfffffff0);
        assert(!ui_alwatch(&gp,r) && !writes && !uinter_patched);
        assert(!(gp.gp_flags & GPF_DONE(0)) && GR_PC(r)==0x10012); checks++;
    }
    /* Both skip branches must load qLink before the back edge. */
    setup(&gp,r,0); assert(!ui_alwatch(&gp,r));
    assert(16+(signed char)memory[0x10000+HFSWALK+15]==30);
    assert(24+(signed char)memory[0x10000+HFSWALK+23]==30);
    assert(memory[0x10000+HFSWALK+30]==0x20 &&
           memory[0x10000+HFSWALK+31]==0x12); checks++;
    setup(&gp,r,0); P16(memory+0x15a,0x0761);
    assert(!ui_alwatch(&gp,r) && !(gp.gp_flags&GPF_AWATCH));
    assert(!writes && !uinter_patched); checks++;
    setup(&gp,r,0); uinter_patches=0;
    assert(!ui_alwatch(&gp,r) && !(gp.gp_flags&GPF_AWATCH));
    assert(!writes && !uinter_patched); checks++;

    for(i=64;i<=255;i++) {
        unsigned char t[TEXT]; long result=0;
        setup(&gp,r,0); GR_A(r,0)=0x3000;
        P32(memory+0x3000+0x12,0x4000);
        memset(t,0,sizeof t); t[TNAME]=i;
        memcpy(memory+0x5000+FSIZE,t,sizeof t);
        assert(!trashnext(r,MFOLD,0x5000,&result));
        assert(result==FNFERR && !writes); checks++;
        result=0;
        assert(!trashnext(r,MTRASH,0x5000,&result));
        assert(result==FNFERR && !writes); checks++;
        result=0;
        assert(!trashundo(r,t,&result));
        assert(result==FNFERR && !writes); checks++;
        t[TNAME]=5; memcpy(memory+0x5000+FSIZE,t,sizeof t);
        memory[0x4000]=i; result=0;
        assert(!trashnext(r,MTRASH,0x5000,&result));
        assert(result==FNFERR); checks++;
    }
    setup(&gp,r,0); P32(memory+VCBQ,0x2000);
    P32(memory+0x2000,0x2000); P16(memory+0x3000+0x16,-1);
    assert(!extvcb(0x3000,9) && !uxvcb()); checks++;
    P32(memory+0x2000,0x2100); P32(memory+0x2100,0x2000);
    assert(!extvcb(0x3000,9) && !uxvcb()); checks++;
    P32(memory+VCBQ,0xfffffffe);
    assert(!extvcb(0x3000,9) && !uxvcb()); checks++;
    P32(memory+VCBQ,0x2001);
    assert(!extvcb(0x3000,9) && !uxvcb()); checks++;
    P32(memory+VCBQ,0x2000); P32(memory+0x2000,0);
    P16(memory+0x2000+0x4c,0x17); P16(memory+0x2000+0x4e,-1);
    assert(extvcb(0x3000,9)==0x2000 && uxvcb()==0x2000); checks++;
    for(i=0;i<6;i++) {
        uint32_t oldsp,oldpc; unsigned char saved[60];
        setup(&gp,r,0); GR_PC(r)=0x3000; P16(memory+0x3000,0xa860);
        if(i==1) P16(memory+0x15a,0x0761);
        if(i==2) P16(memory+0x15a,0x0608);
        if(i==3) P16(memory+0x15a,0x0811);
        if(i==4) uinter_patches=0;
        if(i==5) gp.gp_flags |= GPF_STUB;
        forcequeue=1; oldsp=GR_USP(r); oldpc=GR_PC(r);
        if(i) {
            assert(!ui_alwatch(&gp,r)); assert(forcequeue==(i==5) && !writes);
            assert(GR_USP(r)==oldsp && GR_PC(r)==oldpc);
        } else {
            long f;
            GR_D(r,0)=0x12345678; GR_A(r,6)=0x6789abcd;
            memcpy(saved,r+4,60);
            assert(ui_alwatch(&gp,r) && !forcequeue);
            f=GR_USP(r); assert(GR_PC(r)==f+4 && (gp.gp_flags&GPF_STUB));
            assert(G16(memory+f+6)==FSIZE-6);
            assert(!memcmp(memory+f+FSIZE,forcecode,sizeof forcecode));
            GR_PC(r)=f+16; GR_D(r,0)=1;
            assert(ui_alwatch(&gp,r));
            assert(!memcmp(saved,r+4,60) && GR_PC(r)==oldpc && GR_USP(r)==oldsp);
            assert(!forcequeue && !(gp.gp_flags&GPF_STUB));
            forcequeue=1; assert(ui_alwatch(&gp,r)); f=GR_USP(r);
            GR_PC(r)=f+16; GR_D(r,0)=0;
            assert(ui_alwatch(&gp,r) && forcequeue);
            assert(GR_PC(r)==oldpc && GR_USP(r)==oldsp);
        }
        uinter_patches=1; checks++;
    }
    {
      int errors[]={0,3,20002,32767},returned;
      for(i=0;i<4;i++) for(returned=0;returned<2;returned++) {
        long f;
        setup(&gp,r,0); forcequeue=1; GR_PC(r)=0x3000;
        P16(memory+0x3000,0xa860); assert(ui_alwatch(&gp,r)); f=GR_USP(r);
        P16(memory+0xaf0,errors[i]); GR_PC(r)=f+16; GR_D(r,0)=returned;
        assert(ui_alwatch(&gp,r));
        assert(G16(memory+0xaf0)==(returned && errors[i]==20002 ? 0 : errors[i])); checks++;
      }
      for(i=0;i<2;i++) {
        long f; int before=signals;
        setup(&gp,r,0); forcequeue=1; GR_PC(r)=0x3000;
        P16(memory+0x3000,0xa860); assert(ui_alwatch(&gp,r)); f=GR_USP(r);
        P16(memory+0xaf0,20002); GR_PC(r)=f+16; GR_D(r,0)=1;
        if(i) {denyoutaddr=0xaf0;denyoutnth=1;outseen=0;}else denyinaddr=0xaf0;
        assert(ui_alwatch(&gp,r) && signals==before+1 && G16(memory+0xaf0)==20002); checks++;
        denyinaddr=denyoutaddr=0;denyoutnth=0;
      }
    }
    setup(&gp,r,0); forcequeue=1; deny=1; GR_PC(r)=0x3000;
    assert(!ui_forcecall(r) && forcequeue && GR_PC(r)==0x3000 && GR_USP(r)==0x80000);
    checks++;
    setup(&gp,r,0); gp.gp_flags |= GPF_DONE(0);
    GR_PC(r)=0x3000; P16(memory+0x3000,0xa860);
    assert(!ui_alwatch(&gp,r) && !(gp.gp_flags&GPF_AWATCH)); checks++;
    forcequeue=1; gp.gp_flags|=GPF_AWATCH;
    assert(ui_alwatch(&gp,r) && (gp.gp_flags&GPF_STUB)); checks++;
    {
        long f=GR_USP(r); GR_PC(r)=0x3000; P16(memory+0x3000,0xa9f4);
        assert(!ui_alwatch(&gp,r) && !(gp.gp_flags&(GPF_AWATCH|GPF_STUB)));
        assert(GR_USP(r)==f); checks++;
    }
    memset(memory,0,sizeof memory); deny=0; writes=0;
    ui.l_romaddr=(caddr_t)0x70000;
    memcpy(memory+0x70000+ROMDOWN+6,pollmiddle,sizeof pollmiddle);
    memcpy(memory+0x70000+ROMDOWN,polldown,6); memcpy(memory+0x70000+ROMUP,pollup,6);
    memcpy(memory+0x70000+ROMHIT,hitnative,sizeof hitnative);
    P16(memory+0x15a,0x0810); P32(memory+WNEENT,0x3000);
    P32(memory+0x904,0x4000); memcpy(memory+0x3000,wnenative,sizeof wnenative);
    for(i=0;i<sizeof wnenative;i++) {
        memory[0x3000+i]^=1; writes=0;
        assert(!ui_forceentry(0x5000) && !writes); checks++;
        memory[0x3000+i]^=1;
    }
    P16(memory+0x15a,0x0811); writes=0;
    assert(!ui_forceentry(0x5000) && !writes); checks++;
    P16(memory+0x15a,0x0810); P32(memory+0x904,0);
    assert(!ui_forceentry(0x5000) && !writes); checks++;
    P32(memory+0x904,0x4001); assert(!ui_forceentry(0x5000) && !writes); checks++;
    P32(memory+0x904,0x4000);
    for(i=0;i<3;i++) {
        uintptr_t a=i==0 ? 0x5000 : i==1 ? 0x3000 : 0x5000+WNEWORD;
        P32(memory+WNEENT,0x3000); memcpy(memory+0x3000,wnenative,sizeof wnenative); denyoutaddr=a;
        assert(!ui_forceentry(0x5000));
        assert(G32(memory+WNEENT)==0x3000);
        assert(G16(memory+0x3000)==(i==2 ? 0x4ef9 : G16(wnenative))); checks++;
    }
    denyoutaddr=0; P32(memory+WNEENT,0x3000); memcpy(memory+0x3000,wnenative,sizeof wnenative);
    denyoutaddr=0x3000; denyoutnth=2; outseen=0; partialout=1;
    assert(!ui_forceentry(0x5000) && !memcmp(memory+0x3000,wnenative,sizeof wnenative)); checks++;
    denyoutaddr=0; denyoutnth=partialout=0;
    denyprot=1; assert(!ui_forceentry(0x5000)); checks++; denyprot=0;
    uin_polldiag=1;
    assert(ui_forceentry(0x5000)==1 && G32(memory+WNEENT)==0x3000 && G32(memory+0x3002)==0x5000);
    assert(G32(memory+0x5000+WNELINK)==0x5000+sizeof wneentry && G32(memory+0x5000+WNEWORD)==1); checks++;
    assert(G32(memory+0x5000+sizeof wneentry+14+POLLDIAG)==1); checks++;
    uin_polldiag=0;
    P32(memory+0x5000+WNEWORD,3); writes=0;
    assert(ui_forceentry(0x5000)==2 && !writes); checks++;
    assert(uin_forceentry==0x5000 && uin_forceword==3); checks++;
    assert(G32(memory+0x5000+sizeof wneentry+14+POLLDIAG)==1); checks++;
    uin_polldiag=1;
    P32(memory+0x5000+WNEWORD,4);
    denyoutaddr=0x70000+ROMUP;
    assert(!ui_forceentry(0x5000) && G32(memory+0x5000+WNEWORD)==4); checks++;
    denyoutaddr=0; assert(ui_forceentry(0x5000)==1); checks++;
    P32(memory+0x5000+WNEWORD,0);
    for(i=0;i<WNEWORD;i++) {
        if(i>=WNELINK && i<WNELINK+4) continue;
        memory[0x5000+i]^=1; writes=0;
        assert(!ui_forceentry(0x5000) && !writes); checks++;
        memory[0x5000+i]^=1;
    }
    assert(ui_forceentry(0x5000)==1 && writes>0); checks++;
    writes=0; assert(ui_forceentry(0)==1 && writes>0); checks++;
    writes=0; assert(ui_forceentry(0x6000)==1 && writes>0 && G32(memory+WNEENT)==0x3000); checks++;
    {
      struct guest_proc g,other; char regs[100]={0}; long a=0x5000,p=a+sizeof wneentry+14;
      ui.l_active=&g; ui.l_task[0].t_gp=&g; ui.l_task[0].t_forceentry=a;
      GR_PC(regs)=p+POLLTRAP; P32(memory+a+WNEWORD,3);
      assert(ui_forcepoll(&other,regs,0)==EINVAL && !inputs); checks++;
      GR_PC(regs)++; assert(ui_forcepoll(&g,regs,0)==EINVAL && !inputs); checks++; GR_PC(regs)--;
      for(i=0;i<sizeof pollentry;i++) {
        if(i==POLLDIAG+3) continue;
        memory[p+i]^=1;
        assert(ui_forcepoll(&g,regs,0)==EINVAL && !inputs); checks++;
        memory[p+i]^=1;
      }
      assert(ui_forcepoll(&g,regs,0)==0 && inputs==1); checks++;
      GR_PC(regs)=a+WNELEAVE;
      assert(ui_forcepoll(&g,regs,1)==0 && !memcmp(memory+0x70000+ROMDOWN,polldown,6) &&
        !memcmp(memory+0x70000+ROMUP,pollup,6)); checks++;
      assert(ui_forcepoll(&g,regs,1)==0); checks++;
      for(i=0;i<6;i++) {
        memory[0x70000+ROMDOWN+i]^=1; writes=0;
        assert(!forcepollinstall(a) && !writes); checks++;
        memory[0x70000+ROMDOWN+i]^=1;
      }
      for(i=0;i<sizeof pollmiddle;i++) {
        memory[0x70000+ROMDOWN+6+i]^=1; writes=0;
        assert(!forcepollinstall(a) && !writes); checks++;
        memory[0x70000+ROMDOWN+6+i]^=1;
      }
      for(i=0;i<3;i++) {
        denyoutaddr=0x70000+(i==0 ? ROMDOWN : i==1 ? ROMUP : ROMHIT); denyoutnth=1; outseen=0; partialout=1;
        assert(!forcepollinstall(a));
        assert(!memcmp(memory+0x70000+ROMDOWN,polldown,6) && !memcmp(memory+0x70000+ROMUP,pollup,6) && !memcmp(memory+0x70000+ROMHIT,hitnative,sizeof hitnative)); checks++;
      }
      denyoutaddr=0; denyoutnth=partialout=0;
      assert(forcepollinstall(a)); checks++;
      GR_PC(regs)=a+WNELEAVE; denyoutaddr=0x70000+ROMUP;
      assert(ui_forcepoll(&g,regs,1)==EFAULT && !memcmp(memory+0x70000+ROMDOWN,polldown,6)); checks++;
      denyoutaddr=0; assert(ui_forcepoll(&g,regs,1)==0); checks++;
      for(i=0;i<sizeof hitnative;i++) {
        memory[0x70000+ROMHIT+i]^=1; writes=0;
        assert(!forcepollinstall(a) && !writes); checks++;
        memory[0x70000+ROMHIT+i]^=1;
      }
      for(i=0;i<3;i++) {
        denyoutaddr=0x70000+(i==0 ? ROMDOWN : i==1 ? ROMUP : ROMHIT);
        denyoutnth=2; outseen=0; partialout=1;
        assert(!forcepollinstall(a));
        assert(!memcmp(memory+0x70000+ROMDOWN,polldown,6) &&
          !memcmp(memory+0x70000+ROMUP,pollup,6) &&
          !memcmp(memory+0x70000+ROMHIT,hitnative,sizeof hitnative)); checks++;
      }
      denyoutaddr=0; denyoutnth=partialout=0; assert(forcepollinstall(a));
      ui.l_task[0].t_proc=&proc;
      g.gp_proc=&proc;
      P32(memory+a+WNEWORD,3); ui.l_task[0].t_force|=4;
      assert(ui_forceexit(&other)==EINVAL && G32(memory+a+WNEWORD)==3); checks++;
      ui.l_task[0].t_proc=0;
      assert(ui_forceexit(&g)==EINVAL && G32(memory+a+WNEWORD)==3); checks++;
      ui.l_task[0].t_proc=&proc;
      memory[p]^=1; writes=0;
      assert(ui_forceexit(&g)==EINVAL && !writes); checks++; memory[p]^=1;
      memory[a]^=1; writes=0;
      assert(ui_forceexit(&g)==EINVAL && !writes); checks++; memory[a]^=1;
      memory[0x70000+ROMDOWN]^=1; writes=0;
      assert(ui_forceexit(&g)==EINVAL && !writes && G32(memory+a+WNEWORD)==3); checks++;
      memory[0x70000+ROMDOWN]^=1;
      memcpy(memory+0x6000,memory+a,sizeof wneentry+14);
      P32(memory+0x6000+WNELINK,0x6000+sizeof wneentry);
      P32(memory+0x3002,0x6000); writes=0;
      assert(ui_forceexit(&g)==EINVAL && !writes && G32(memory+a+WNEWORD)==3); checks++;
      P32(memory+0x3002,a);
      for(i=0;i<5;i++) {
        P32(memory+a+WNEWORD,3); ui.l_task[0].t_force|=4;
        assert(forcepollinstall(a));
        denyoutaddr=i==0 || i==4 ? a+WNEWORD : 0x70000+(i==1 ? ROMDOWN : i==2 ? ROMUP : ROMHIT);
        denyoutnth=i==4 ? 2 : 1; outseen=0;
        assert(ui_forceexit(&g)==EFAULT && G32(memory+a+WNEWORD)!=0 && (ui.l_task[0].t_force & 4)); checks++;
        denyoutaddr=0; denyoutnth=0;
      }
      for(i=0;i<5;i++) {
        unsigned char saved[80]; int before=signals;
        P32(memory+a+WNEWORD,3); ui.l_task[0].t_force|=4;
        assert(forcepollinstall(a));
        denyoutaddr=i==0 || i==4 ? a+WNEWORD : 0x70000+(i==1 ? ROMDOWN : i==2 ? ROMUP : ROMHIT);
        denyoutnth=i==4 ? 2 : 1; outseen=0;
        GR_PC(regs)=0x4502; P16(memory+0x4502,0xa9f4); memcpy(saved,regs,80);
        assert(ui_alwatch(&g,regs)==1 && signals==before+1);
        assert(!memcmp(saved,regs,80) && G32(memory+a+WNEWORD)!=0 && (ui.l_task[0].t_force & 4)); checks++;
        denyoutaddr=0; denyoutnth=0;
      }
      {
        int errors[]={0,3,20002,32767};
        for(i=0;i<4;i++) {
          P16(memory+0xaf0,errors[i]); P32(memory+a+WNEWORD,3);
          ui.l_task[0].t_force|=4; assert(forcepollinstall(a));
          assert(ui_forceexit(&g)==0 && G16(memory+0xaf0)==(errors[i]==20002 ? 0 : errors[i])); checks++;
        }
        P16(memory+0xaf0,20002); P32(memory+a+WNEWORD,3);
        ui.l_task[0].t_force|=4; assert(forcepollinstall(a));
        denyinaddr=0xaf0; writes=0;
        assert(ui_forceexit(&g)==EFAULT && !writes && G32(memory+a+WNEWORD)==3); checks++;
        denyinaddr=0;
        denyoutaddr=0xaf0; denyoutnth=1; outseen=0;
        assert(ui_forceexit(&g)==EFAULT && G16(memory+0xaf0)==20002 && (ui.l_task[0].t_force & 4)); checks++;
        denyoutaddr=0; denyoutnth=0; P16(memory+0xaf0,0);
      }
      P32(memory+a+WNEWORD,3); assert(forcepollinstall(a));
      assert(ui_forceexit(&g)==0 && G32(memory+a+WNEWORD)==0 && !(ui.l_task[0].t_force & 4)); checks++;
      assert(ui_forceentry(a)==1 && G32(memory+a+WNEWORD)==1); checks++;
      P32(memory+a+WNEWORD,3); ui.l_task[0].t_force|=4;
      g.gp_flags=GPF_AWATCH | (GPF_DONE(NPATCH)-GPF_DONE(0));
      GR_PC(regs)=0x4500; P16(memory+0x4500,0xa8ad);
      assert(!ui_alwatch(&g,regs) && (g.gp_flags & GPF_AWATCH) && GR_PC(regs)==0x4500); checks++;
      {
        unsigned char saved[80];
        GR_PC(regs)=0x4502; P16(memory+0x4502,0xa9f4); memcpy(saved,regs,80);
        assert(!ui_alwatch(&g,regs) && G32(memory+a+WNEWORD)==0 && !(ui.l_task[0].t_force & 4));
        assert(!memcmp(saved,regs,80) && !(g.gp_flags & GPF_AWATCH)); checks++;
        {
          int before=signals;
          assert(!ui_alwatch(&g,regs) && signals==before && !memcmp(saved,regs,80)); checks++;
        }
      }
      {
        int errors[]={0,3,20002,32767};
        for(i=0;i<4;i++) {
          P16(memory+0xaf0,errors[i]); P32(memory+a+WNEWORD,3);
          ui.l_task[0].t_force|=4; assert(forcepollinstall(a));
          GR_PC(regs)=a+WNELEAVE;
          assert(ui_forcepoll(&g,regs,1)==0 && G16(memory+0xaf0)==(errors[i]==20002 ? 0 : errors[i])); checks++;
        }
        P16(memory+0xaf0,20002); P32(memory+a+WNEWORD,3);
        ui.l_task[0].t_force|=4; assert(forcepollinstall(a)); GR_PC(regs)=a+WNELEAVE;
        denyinaddr=0xaf0; writes=0;
        assert(ui_forcepoll(&g,regs,1)==EFAULT && !writes && G16(memory+0xaf0)==20002); checks++;
        denyinaddr=0;
        denyoutaddr=0xaf0; denyoutnth=1; outseen=0;
        assert(ui_forcepoll(&g,regs,1)==EFAULT && G16(memory+0xaf0)==20002 && (ui.l_task[0].t_force & 4)); checks++;
        denyoutaddr=0; denyoutnth=0; P16(memory+0xaf0,0); P32(memory+a+WNEWORD,0);
      }
      assert(ui_forceentry(a)==1); P32(memory+a+WNEWORD,3);

      uin_polldiag=1; GR_USP(regs)=0x90000; GR_A(regs,3)=0x2000;
      P32(memory+0x90000+78,0x00a601b3); P32(memory+0x2000,0x009a019b);
      P32(memory+0x2004,0x00ae01cf); memory[0x172]=0;
      GR_PC(regs)=p+POLLSTATEPC;
      P32(memory+0x90000+12,0); P32(memory+0x90000+80,0x702a3e);
      assert(ui_forcepoll(&other,regs,4)==EINVAL && uin_pollcalls[4]==1 && uin_pollstatelast[2]==1); checks++;
      GR_PC(regs)++;
      assert(ui_forcepoll(&g,regs,4)==EINVAL && uin_pollcalls[4]==2 && uin_pollstatelast[2]==2); checks++;
      GR_PC(regs)--; memory[p]^=1;
      assert(ui_forcepoll(&g,regs,4)==EINVAL && uin_pollcalls[4]==3 && uin_pollstatelast[2]==3); checks++;
      memory[p]^=1;
      assert(ui_forcepoll(&g,regs,4)==0 && uin_pollnative[0]==1 &&
        uin_pollnative[1]==0 && uin_pollnative[2]==0x702a3e && uin_pollnative[3]==1); checks++;
      assert(uin_pollcalls[4]==4 && uin_pollstatelast[2]==4); checks++;
      P32(memory+0x90000+12,0x80);
      assert(ui_forcepoll(&g,regs,4)==0 && uin_pollnative[0]==2 &&
        uin_pollnative[1]==0x80 && uin_pollnative[3]==1); checks++;
      P32(memory+0x90000+78,0x00a601b3);
      uin_forcehitcalls[0]=0;
      GR_PC(regs)=p+HITPOINTPC;
      assert(ui_forcepoll(&other,regs,2)==EINVAL && uin_forcehitcalls[0]==1 &&
        uin_forcehitlast[3]==1 && !uin_forcehitcount); checks++;
      GR_PC(regs)++;
      assert(ui_forcepoll(&g,regs,2)==EINVAL && uin_forcehitcalls[0]==2 &&
        uin_forcehitlast[0]==p+HITPOINTPC+1 && uin_forcehitlast[1]==p+HITPOINTPC &&
        uin_forcehitlast[3]==2 && !uin_forcehitcount); checks++;
      GR_PC(regs)--; memory[p]^=1;
      assert(ui_forcepoll(&g,regs,2)==EINVAL && uin_forcehitcalls[0]==3 &&
        uin_forcehitlast[3]==3 && !uin_forcehitcount); checks++;
      memory[p]^=1;
      assert(ui_forcepoll(&g,regs,2)==0 && uin_forcehitcalls[0]==4 &&
        uin_forcehitlast[3]==4 && uin_forcehitcount==1); checks++;
      uin_forcehitcount=0;
      GR_PC(regs)=p+HITPOINTPC;
      assert(ui_forcepoll(&g,regs,2)==0 && uin_forcehitcount==1 &&
        uin_forcehits[0][0]==0x00a601b3 && uin_forcehits[0][2]==0x009a019b); checks++;
      P16(memory+0x90000+16,0x10); P32(memory+0x90000+18,0x10001);
      GR_PC(regs)=p+HITRESULTPC;
      assert(ui_forcepoll(&g,regs,3)==0 && uin_forcehits[0][5]==0x10001 &&
        uin_forcehits[0][6]==0x10 && !uin_forcehitpending); checks++;
      for(i=0;i<35;i++) {
        GR_PC(regs)=p+HITPOINTPC; assert(ui_forcepoll(&g,regs,2)==0);
        GR_PC(regs)=p+HITRESULTPC; assert(ui_forcepoll(&g,regs,3)==0);
      }
      assert(uin_forcehitcount==32 && !uin_forcehitpending); checks++;
      uin_forcehitcount=0; GR_USP(regs)=sizeof memory; GR_PC(regs)=p+HITPOINTPC;
      assert(ui_forcepoll(&g,regs,2)==0 && !uin_forcehitcount); checks++;
      uin_polldiag=0; GR_USP(regs)=0x90000;
      assert(ui_forcepoll(&g,regs,2)==0 && !uin_forcehitcount); checks++;
      GR_PC(regs)=a+WNELEAVE; assert(ui_forcepoll(&g,regs,1)==0);
    }

    {
      struct guest_proc g={&proc,0,0}; char regs[100]={0};long a=0x5000,fp;
      unsigned char saved[256];int j;
      memset(memory,0,sizeof memory);proc.p_pid=80;
      ui.l_gp=ui.l_active=&g;ui.l_task[0].t_gp=&g;ui.l_task[0].t_forceentry=a;
      ui.l_task[1].t_gp=0;uin_frontdiag=0;ui_frontclear();writes=0;
      P16(memory+0x15a,0x0810);P32(memory+WNEENT,0x3000);
      memcpy(memory+0x3000,wnenative,sizeof wnenative);P32(memory+0x904,0x4000);P32(memory+0x4000,1);
      ui_fronttick(&g);assert(!writes && !uin_frontflag);checks++;
      uin_frontdiag=1;ui_fronttick(&g);
      assert(uin_frontflag==a+FRONTEXT+FRONTFLAG && uin_frontpid==80 &&
          G32(memory+a+WNEWORD)==0 && !memcmp(memory+0x3008,wnenative+8,sizeof wnenative-8));checks++;
      fp=0x90000;GR_USP(regs)=fp-16;GR_PC(regs)=a+FRONTEXT+FRONTTRAP;
      P32(memory+fp-4,fp);P32(memory+fp,0);P32(memory+fp+4,8);
      P32(memory+fp+8,0);P32(memory+fp+12,8);P32(memory+fp+16,0x6000);P32(memory+fp+20,0x4000);
      memory[0x6000+110]=1;memory[fp+24]=10;memcpy(memory+fp+25,"Appearance",10);
      assert(ui_forcepoll(&g,regs,5)==0 && uin_frontcount==1 && !strcmp((char*)uin_fronttitle,"Appearance"));checks++;
      memcpy(saved,uin_fronttitle,256);writes=0;
      GR_PC(regs)++;assert(ui_forcepoll(&g,regs,5)==EINVAL && !writes);checks++;GR_PC(regs)--;
      proc.p_pid++;assert(ui_forcepoll(&g,regs,5)==EINVAL && !writes);checks++;proc.p_pid--;
      for(j=0;j<FRONTFLAG;j++) {
        memory[a+FRONTEXT+j]^=1;assert(ui_forcepoll(&g,regs,5)==EINVAL && !writes);checks++;memory[a+FRONTEXT+j]^=1;
      }
      P32(memory+fp+12,9);assert(ui_forcepoll(&g,regs,5)==EINVAL && !writes);checks++;P32(memory+fp+12,8);
      P32(memory+fp+20,0x4002);assert(ui_forcepoll(&g,regs,5)==EINVAL && !writes);checks++;P32(memory+fp+20,0x4000);
      P32(memory+fp-4,fp+4);assert(ui_forcepoll(&g,regs,5)==EINVAL && !writes);checks++;P32(memory+fp-4,fp);
      memory[0x6000+110]=0;assert(ui_forcepoll(&g,regs,5)==EINVAL && !writes);checks++;memory[0x6000+110]=1;
      memory[0x3000]^=1;assert(ui_forcepoll(&g,regs,5)==EINVAL && !writes);checks++;memory[0x3000]^=1;
      denyinaddr=fp;assert(ui_forcepoll(&g,regs,5)==EINVAL && !writes);checks++;denyinaddr=0;
      assert(!memcmp(saved,uin_fronttitle,256) && uin_frontcount==1);checks++;
      memory[fp+24]=255;memset(memory+fp+25,'x',255);
      assert(ui_forcepoll(&g,regs,5)==0 && strlen((char*)uin_fronttitle)==255);checks++;
      uin_frontdiag=0;assert(ui_forcepoll(&g,regs,5)==0 && !G32(memory+uin_frontflag));checks++;
      ui_frontclear();assert(!uin_frontflag && !uin_frontpid && !uin_frontcount && !uin_fronttitle[0]);checks++;
    }
    printf("uipatch: %d checks passed\n",checks);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='uipatch-') as tmp:
    c = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    c.write_text(header + source.read_text().replace('#include "uinter.h"', '').replace('#include "sys/mman.h"', '').replace('#include "sys/resource.h"', '').replace('#include "vm/as.h"', '') + tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu89', '-w', '-fno-strict-aliasing', '-fsanitize=address,undefined', '-fno-sanitize=alignment', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    subprocess.run([sys.executable, str(Path(__file__).with_name("t_aslmload.py")),
                    str(source), str(Path(tmp) / "aslm")], check=True)
