#include <assert.h>
#include <stdio.h>
#include <string.h>
unsigned long OTFindConfigurator_binding[1],__aslm_ctx=0x6000;
unsigned char otr_admission_tail[38],otr_admission_helper[16],otr_admission_lo[1],otr_admission_hi[1];
static int opens,closes,ioctls,resolves,resolvefail,openfail,checks,holds,releases,holdfail,owner,contextfail,queryfail,recursive;
static long ioctl_error;
extern long otr_admission_register(void);
unsigned char OTFindConfigurator(void *output,void *c) {assert(output && !c);++resolves;OTFindConfigurator_binding[0]=resolvefail ? 0 : 0x2000;return 0;}
long otr_sys_open(const char *name,long flags) {++opens;assert(!strcmp(name,"/dev/uinter0") && flags==2);return openfail ? -13 : 3;}
long otr_sys_close(long fd) {++closes;assert(fd==3);return 0;}
long otr_sys_ioctl(long fd,unsigned long cmd,void *p) {
    unsigned long *a=p;++ioctls;assert(fd==3);
    if(cmd==0xc0085192UL) {assert(a[0]==__aslm_ctx);if(contextfail || (queryfail && resolves))return -14;a[1]=owner;return 0;}
    assert(cmd==0x80145191UL && a[0]==0x2000 && a[1]==(unsigned long)otr_admission_tail && a[2]==(unsigned long)otr_admission_helper && a[3]==(unsigned long)otr_admission_lo && a[4]==(unsigned long)otr_admission_hi-a[3]);return ioctl_error;
}
long otr_admission_hold(const char *name) {
    ++holds;assert(!strcmp(name,holds==1 ? "OTClnt$NtvUtils" : "OTClnt$auxCfig"));
    if(recursive)assert(otr_admission_register()==-16);
    return holdfail==holds ? -3120 : 0;
}
void otr_admission_release(const char *name) {assert(!strcmp(name,"OTClnt$NtvUtils") || !strcmp(name,"OTClnt$auxCfig"));++releases;}
#include "../admission-register.c"
#define C(x) do {assert(x);++checks;} while(0)
static void reset(void) {admissionphase=0;opens=closes=ioctls=resolves=resolvefail=openfail=holds=releases=holdfail=owner=contextfail=queryfail=recursive=0;ioctl_error=0;}
int main(void)
{
    reset();openfail=1;C(otr_admission_register()==-13);C(!closes && !ioctls && !holds && !admissionphase);
    reset();contextfail=1;C(otr_admission_register()==-14);C(closes==1 && !holds && !admissionphase);
    reset();holdfail=1;C(otr_admission_register()==-3120);C(holds==1 && !releases && !admissionphase);
    reset();holdfail=2;C(otr_admission_register()==-3120);C(holds==2 && releases==1 && !admissionphase);
    reset();resolvefail=1;C(otr_admission_register()==-22);C(holds==2 && releases==2 && !admissionphase);
    reset();ioctl_error=-22;C(otr_admission_register()==-22);C(holds==2 && releases==2 && !admissionphase);
    reset();ioctl_error=-14;owner=1;C(otr_admission_register()==-14);C(holds==2 && !releases && admissionphase==2);
    ioctl_error=0;C(!otr_admission_register());C(holds==2 && !releases && admissionphase==2);
    reset();ioctl_error=-14;queryfail=1;C(otr_admission_register()==-14);C(holds==2 && !releases && admissionphase==2);
    reset();recursive=1;C(!otr_admission_register());C(holds==2 && opens==1 && admissionphase==2);
    C(!otr_admission_register());C(holds==2 && opens==2 && closes==2 && admissionphase==2);
    printf("%d actual bootstrap/hold/rollback checks PASS\n",checks);return 0;
}
