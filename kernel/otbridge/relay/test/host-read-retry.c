#include "../host.h"
#include <errno.h>
extern int printf(const char *, ...);
extern void exit(int);
static unsigned char wire[128];
static unsigned long ready;
static long idle;
static int checks, delivered, closed;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL retry line%d\n",__LINE__); exit(1); } } while(0)
long otr_sys_select(int n,void*r,void*w,void*x,void*t) {(void)n;(void)r;(void)w;(void)x;(void)t;return 0;}
long otr_sys_open(const char *p,int f) {(void)p;(void)f;return -1;}
long otr_sys_ioctl(int f,int c,long a) {(void)f;(void)c;(void)a;return -1;}
long otr_sys_close(int f) {CHECK(f==11);closed++;return 0;}
long otr_sys_write(int f,const unsigned char*p,unsigned long n) {(void)f;(void)p;return n;}
long otr_sys_read(int f,unsigned char*p,unsigned long n) {
    unsigned long i;
    CHECK(f==11);
    if(!ready)return idle;
    CHECK(n>=ready);
    for(i=0;i<ready;i++)p[i]=wire[i];
    i=ready;ready=0;return i;
}
static void release(void*a,struct otr_msg*m) {(void)a;(void)m;CHECK(0);}
static int deliver(void*a,void*q,int t,const unsigned char*c,unsigned long n,const unsigned char*d,unsigned long z) {
    (void)a;(void)q;(void)d;
    CHECK(t==OTB_R_PCPROTO&&n==44&&c[3]==129&&z==0);
    delivered++;return OTR_DONE;
}
static const struct otr_provider_ops ops={otr_host_read,otr_host_write,otr_host_close,release,deliver};
int main(void) {
    struct otr_provider p={0};
    unsigned char ack[44]={0};
    long retry[3]={-55,-EAGAIN,-EINTR};
    int i;
    ack[3]=129;
    for(i=0;i<3;i++) {
        idle=retry[i];delivered=closed=0;
        CHECK(otr_provider_open(&p,&ops,0,&p,11)==OTR_DONE);
        ready=otr_pack(wire,sizeof(wire),OTB_R_PCPROTO,ack,sizeof(ack),0,0);
        CHECK(ready>0);
        CHECK(otr_provider_pump(&p,8)==OTR_DONE);
        CHECK(delivered==1&&p.live&&closed==0);
        CHECK(otr_provider_pump(&p,8)==OTR_WAIT);
        CHECK(p.live&&closed==0);
        idle=-EIO;
        CHECK(otr_provider_pump(&p,8)==OTR_BAD);
        CHECK(!p.live&&closed==1);
    }
    printf("PASS %d actual host/provider read-retry checks\n",checks);return 0;
}
