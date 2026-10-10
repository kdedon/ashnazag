#include <assert.h>
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
static ssize_t mock_read(int,void *,size_t);
static ssize_t mock_write(int,const void *,size_t);
static int mock_close(int),mock_kill(pid_t,int),mock_seteuid(uid_t);
static int mock_open(const char *,int,...),mock_ioctl(int,unsigned long,...);
static int mock_gettimeofday(struct timeval *,void *);
#define SIGEMT 7
#define I_NREAD 0x5301
#define read mock_read
#define write mock_write
#define close mock_close
#define kill mock_kill
#define seteuid mock_seteuid
#define open mock_open
#define ioctl mock_ioctl
#define gettimeofday mock_gettimeofday
#define main relay_main
#include "../sndaux.c"
#undef main
static int result,readerr,writeerr,nclose,nsignal,nspace,nopen,openresult=-1,ioctlerr;
static long clocksec=100;
static struct sndrec incoming;
static ssize_t mock_read(int fd,void *p,size_t n){
	if(result<0){
		errno=readerr;
		return -1;
	}
	if(result>0){
		memcpy(p,&incoming,sizeof incoming);
		return sizeof incoming;
	}
	return 0;
}
static ssize_t mock_write(int fd,const void *p,size_t n){
	if(fd==srv){
		const struct sarep *r=p;
		if(r->p_ret==SAP_SPACE)nspace++;
		return n;
	}
	if(writeerr){
		errno=writeerr;
		return -1;
	}
	return n;
}
static int mock_gettimeofday(struct timeval *t,void *z){
	t->tv_sec=clocksec;
	t->tv_usec=0;
	return 0;
}
static int mock_open(const char *path,int flags,...){
	assert(flags&O_NDELAY);
	nopen++;
	return openresult;
}
static int mock_ioctl(int fd,unsigned long cmd,...){
	return ioctlerr?-1:0;
}
static int mock_close(int fd){
	nclose++;
	return 0;
}
static int mock_kill(pid_t p,int s){
	nsignal++;
	return 0;
}
static int mock_seteuid(uid_t u){
	return 0;
}
static struct ch *setup(void){
	struct sareq q;
	struct ch *c;
	memset(&q,0,sizeof q);
	q.q_ch=5;
	assert(open1(&q)==0);
	c=bych(5);
	assert(c);
	c->c_fd=4;
	c->c_tn=2;
	c->c_tsent=1;
	c->c_tag[0].t_cb=c->c_tag[1].t_cb=1;
	c->c_tag[0].t_cmd[0]=17;
	c->c_tag[1].t_cmd[0]=23;
	c->c_wait=1;
	nclose=nsignal=nspace=0;
	writeerr=0;
	return c;
}
int main(void){
	struct ch *c;
	int i;
	srv=3;
	c=setup();
	result=-1;
	readerr=EINTR;
	replies(c);
	assert(c->c_fd==4&&c->c_tn==2&&nsignal==0&&nclose==0);
	readerr=EAGAIN;
	replies(c);
	assert(c->c_fd==4&&c->c_tn==2);
	release(c);
	for(i=0;i<3;i++){
		c=setup();
		result=i==0?0:-1;
		readerr=i==1?EIO:EPIPE;
		replies(c);
		assert(c->c_used&&c->c_fd==-1&&c->c_cbn==2&&c->c_tn==0&&c->c_tsent==0);
		assert(c->c_cb[0][0]==17&&c->c_cb[1][0]==23);
		assert(nsignal==1&&nspace==1&&nclose==1);
		release(c);
	}
	c=setup();
	c->c_closed=1;
	result=0;
	replies(c);
	assert(!c->c_used&&nclose==1&&nsignal==0);
	c=setup();
	result=1;
	incoming.r_cmd=SNDR_DRAIN;
	incoming.r_len=~0UL;
	replies(c);
	assert(c->c_fd==-1&&c->c_cbn==2);
	release(c);
	c=setup();
	result=1;
	incoming.r_cmd=SNDR_DRAIN;
	incoming.r_len=0;
	replies(c);
	assert(c->c_fd==4&&c->c_cbn==1&&c->c_tn==1&&c->c_tsent==0&&c->c_cb[0][0]==17);
	release(c);
	c=setup();
	rec(c,SNDR_DRAIN,0,0);
	writeerr=EPIPE;
	send(c);
	assert(c->c_fd==-1&&c->c_cbn==2&&c->c_qn==0&&nspace==1);
	release(c);
	c=setup();
	lost(c);
	nopen=0;
	assert(stream(c)==-1&&nopen==0);
	clocksec++;
	assert(stream(c)==-1&&nopen==1);
	for(i=0;i<20;i++){
		rec(c,SNDR_DRAIN,0,0);
		send(c);
	}
	assert(nopen==1);
	clocksec++;
	openresult=4;
	assert(stream(c)==0&&nopen==2);
	release(c);
	c=setup();
	c->c_fd=-1;
	c->c_retry=0;
	ioctlerr=1;
	nopen=0;
	assert(stream(c)==-1&&c->c_fd==-1&&nopen==1&&nclose==1);
	release(c);
	{
		struct sareq q;
		unsigned char cmd[8], got[8];
		int out, wave;
		memset(&q, 0, sizeof q);
		for (wave = 0; wave < 2; wave++) {
			c = setup();
			c->c_tn = c->c_tsent = 0;
			for (i = 0; i < NCB; i++) {
				memset(cmd, 0, sizeof cmd);
				cmd[1] = 13;
				cmd[7] = i + wave * NCB;
				q.q_cmd = SND_CMD_Q;
				q.q_len = 8;
				assert(ioc(c, &q, cmd, &out) == 0);
			}
			{
				long bytes = c->c_qn;
				assert(ioc(c, &q, cmd, &out) == -EAGAIN);
				assert(c->c_qn == bytes && c->c_tn == NCB);
			}
			q.q_cmd = SND_QFULL;
			assert(ioc(c, &q, cmd, &out) == 1);
			lost(c);
			assert(c->c_cbn == NCB);
			q.q_cmd = SND_GET_CB;
			for (i = 0; i < NCB; i++) {
				assert(ioc(c, &q, got, &out) == 0 && out == 8);
				assert(got[7] == i + wave * NCB);
			}
			assert(ioc(c, &q, got, &out) == -EAGAIN);
			q.q_cmd = SND_QFULL;
			assert(ioc(c, &q, cmd, &out) == 0);
			q.q_cmd = SND_CMD_Q;
			c->c_qn = QSZ - 4;
			assert(ioc(c, &q, cmd, &out) == -EAGAIN);
			assert(c->c_qn == QSZ - 4 && c->c_tn == 0);
			c->c_qn = 0;
			assert(ioc(c, &q, cmd, &out) == 0);
			release(c);
		}
	}
	puts("PASS relay: EINTR/EAGAIN, EOF/errors, callbacks, blocked writers, malformed replies, drain order, bounded reopen, plain-file refusal, callback capacity");
	return 0;
}
