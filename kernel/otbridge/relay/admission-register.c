#include "admission.h"
extern unsigned char OTFindConfigurator(void *,void *);
extern unsigned long OTFindConfigurator_binding[],__aslm_ctx;
extern unsigned char otr_admission_tail[],otr_admission_helper[];
extern unsigned char otr_admission_lo[],otr_admission_hi[];
extern long otr_sys_open(const char *,long);
extern long otr_sys_close(long);
extern long otr_sys_ioctl(long,unsigned long,void *);
extern long otr_admission_hold(const char *);
extern void otr_admission_release(const char *);
static int admissionphase;

long otr_admission_register(void)
{
    unsigned long arguments[5],context[2];
    unsigned char output[12];
    long fd,result;
    int pinned=admissionphase==2;
    if(admissionphase==1)return -16;
    admissionphase=1;
    fd=otr_sys_open("/dev/uinter0",2);
    if(fd<0) {admissionphase=pinned ? 2 : 0;return fd;}
    context[0]=__aslm_ctx;context[1]=0;
    result=otr_sys_ioctl(fd,0xc0085192UL,context);
    if(result)goto done;
    if(!pinned) {
        result=otr_admission_hold("OTClnt$NtvUtils");
        if(result)goto done;
        result=otr_admission_hold("OTClnt$auxCfig");
        if(result) {otr_admission_release("OTClnt$NtvUtils");goto done;}
        pinned=1;
    }
    (void)OTFindConfigurator(output,0);
    if(!OTFindConfigurator_binding[0]) {result=-22;goto release;}
    arguments[0]=OTFindConfigurator_binding[0];
    arguments[1]=(unsigned long)otr_admission_tail;
    arguments[2]=(unsigned long)otr_admission_helper;
    arguments[3]=(unsigned long)otr_admission_lo;
    arguments[4]=(unsigned long)otr_admission_hi-arguments[3];
    result=otr_sys_ioctl(fd,0x80145191UL,arguments);
    if(!result)goto done;
release:
    context[0]=__aslm_ctx;context[1]=1;
    if(!otr_sys_ioctl(fd,0xc0085192UL,context) && !context[1]) {
        otr_admission_release("OTClnt$auxCfig");
        otr_admission_release("OTClnt$NtvUtils");pinned=0;
    }
done:
    admissionphase=pinned ? 2 : 0;
    (void)otr_sys_close(fd);
    return result;
}
