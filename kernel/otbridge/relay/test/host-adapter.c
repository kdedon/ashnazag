#include "../host.h"
#include <sys/types.h>
#include <sys/stropts.h>
#include <fcntl.h>
#include <errno.h>
extern int printf(const char *, ...);
extern void exit(int);
extern int strcmp(const char *, const char *);
static int count, step, protocol, failed, closes, failstage;
static long response;
#define CHECK(c) do { count++; if (!(c)) { printf("FAIL adapter line %d\n",__LINE__); exit(1); } } while(0)
long otr_sys_open(const char *path,int flags) {
	CHECK(flags==(O_RDWR|O_NONBLOCK));
	if(!step) {CHECK(!strcmp(path,"/dev/otbridge"));step++;return failstage==1?-EIO:10;}
	CHECK(step==2&&!strcmp(path,protocol==OTR_TCP?"/dev/tcp":"/dev/udp"));
	step++;return failstage==3?-EIO:11;
}
long otr_sys_ioctl(int fd,int cmd,long arg) {
	struct strioctl *ioc;struct otx_attach *at;
	if(cmd==I_FLUSH) {CHECK(fd==11&&arg==3);return 0;}
	if(step==1) {CHECK(fd==10&&cmd==OTB_GETID&&arg==0);step++;return failstage==2?-EIO:23;}
	if(step==3) {CHECK(fd==11&&cmd==I_PUSH&&!strcmp((char*)arg,"otxti"));step++;return failstage==4?-EIO:0;}
	if(step==4) {CHECK(fd==11&&cmd==I_SRDOPT&&arg==RMSGN);step++;return failstage==5?-EIO:failed?-EINVAL:0;}
	CHECK(step==5&&fd==11&&cmd==I_STR);step++;
	ioc=(struct strioctl*)arg;at=(struct otx_attach*)ioc->ic_dp;
	CHECK(ioc->ic_cmd==OTX_ATTACH&&ioc->ic_timout==0&&ioc->ic_len==sizeof(*at));
	CHECK(at->at_session==23&&at->at_cookie==17&&at->at_proto==protocol);return failstage==6?-EIO:0;
}
long otr_sys_close(int fd) {CHECK(fd==10||fd==11);closes++;return 0;}
long otr_sys_read(int fd,unsigned char*p,unsigned long n) {(void)fd;(void)p;(void)n;return response;}
long otr_sys_write(int fd,const unsigned char*p,unsigned long n) {(void)fd;(void)p;(void)n;return response;}
static int selectcalls, selectevents;
static long selectresult;
long otr_sys_select(int n,void*r,void*w,void*x,void*t) {
    struct {long seconds,microseconds;} *tv=t;
    unsigned int *mask=r?r:w;
    CHECK(n==12&&!x&&mask[0]==(1U<<11));
    CHECK(tv->seconds==0&&tv->microseconds==100000);
    CHECK((selectevents&1 ? r!=0:r==0)&&(selectevents&2 ? w!=0:w==0));
    selectcalls++;return selectresult;
}
int main(void) {
	struct otr_boot b={0};unsigned char byte=0;
	for(protocol=OTR_TCP;protocol<=OTR_UDP;protocol++) {
		step=closes=0;CHECK(otr_boot_open(&b,&otr_host_boot,0,protocol,17)==OTR_DONE);
		CHECK(step==6);CHECK(otr_host_step==6&&otr_host_result==0);CHECK(otr_host_flush(b.fd,3)==0);otr_boot_close(&b);CHECK(closes==2);
	}
	step=closes=0;failed=1;protocol=OTR_TCP;
	CHECK(otr_boot_open(&b,&otr_host_boot,0,protocol,17)==OTR_BAD);
	CHECK(step==5&&closes==2);CHECK(otr_host_step==5&&otr_host_result==-EINVAL);
	failed=0;
	for(failstage=1;failstage<=6;failstage++) {
		step=closes=0;CHECK(otr_boot_open(&b,&otr_host_boot,0,protocol,17)==OTR_BAD);
		CHECK(otr_host_step==failstage&&otr_host_result==-EIO);
		CHECK(closes==(failstage==1?0:failstage<=3?1:2));
	}
	failstage=0;
	response=-EAGAIN;CHECK(otr_host_read(0,11,&byte,1)==0);CHECK(otr_host_write(0,11,&byte,1)==0);
	response=-55;CHECK(otr_host_read(0,11,&byte,1)==0);CHECK(otr_host_write(0,11,&byte,1)==0);
	response=-56;CHECK(otr_host_read(0,11,&byte,1)==-56);CHECK(otr_host_write(0,11,&byte,1)==-56);
	response=-EINTR;CHECK(otr_host_read(0,11,&byte,1)==0);CHECK(otr_host_write(0,11,&byte,1)==0);
	response=0;CHECK(otr_host_read(0,11,&byte,1)<0);CHECK(otr_host_write(0,11,&byte,1)==0);
	response=-EIO;CHECK(otr_host_read(0,11,&byte,1)==-EIO);CHECK(otr_host_write(0,11,&byte,1)==-EIO);
	response=1;CHECK(otr_host_read(0,11,&byte,1)==1);CHECK(otr_host_write(0,11,&byte,1)==1);
	for(selectevents=1;selectevents<=3;selectevents++) {
        selectresult=1;CHECK(otr_host_wait(11,selectevents,100)==1);
        selectresult=0;CHECK(otr_host_wait(11,selectevents,100)==0);
        selectresult=-EINTR;CHECK(otr_host_wait(11,selectevents,100)==0);
        selectresult=-EIO;CHECK(otr_host_wait(11,selectevents,100)==-EIO);
    }
    CHECK(selectcalls==12);
    CHECK(otr_host_wait(-1,1,100)==-EINVAL && otr_host_wait(1024,1,100)==-EINVAL);
    CHECK(otr_host_wait(11,0,100)==-EINVAL && otr_host_wait(11,4,100)==-EINVAL);
    CHECK(otr_host_wait(11,1,1001)==-EINVAL && selectcalls==12);
	printf("PASS %d host adapter checks\n",count);return 0;
}
