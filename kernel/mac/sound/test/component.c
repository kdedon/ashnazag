#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define long int
#include "../../../../images/macenv/sound/sdev.c"
#undef long
static char *descriptor;
static unsigned stamp=100;
static int pulls,primes,deferred,writes,limit=100000,nclose;
unsigned int ticks(){
	return stamp;
}
int auxsysr(int op,int fd,int p,int n){
	if(op==33)return -1;
	if(op==5)return 3;
	if(op==54)return 0;
	if(op==6){
		nclose++;
		return 0;
	}
	if(op==4){
		writes++;
		if(writes>limit)return -11;
		return n>3?3:n;
	}
	return 0;
}
int splhi(){
	return 0;
}
void splx(int s){
}
int gettrap(){
	return 0;
}
int smversion(){
	return 0;
}
int newhandlesys(){
	return 0;
}
int delegate(){
	return 0;
}
int registercomp(){
	return 0;
}
int findnext(){
	return 0;
}
int capture(){
	return 0;
}
int setdefault(){
	return 0;
}
int getsrc(int m,char **d){
	*d=pulls-->0?descriptor:0;
	return 0;
}
int openmixer(){
	return 0;
}
int closemixer(){
	return 0;
}
void instime(){
}
void primetime(){
	primes++;
}
void dtinstall(){
	deferred++;
}
void p_sdev(){
}
void t_tm(){
}
void t_dt(){
}
int sndnew(){
	return 0;
}
int sndplay(){
	return 0;
}
int snddo(){
	return 0;
}
char *newptr(){
	return 0;
}
int hlock(){
	return 0;
}
void t_cb(){
}
static void reset(void){
	running=tmon=dtq=0;
	head=tail=0;
	hoff=hlen=left=0;
	sfd=-1;
	retry=0;
	needfmt=0;
	primes=deferred=writes=nclose=0;
	limit=100000;
	pulls=0;
	ahead=0;
	last=stamp;
	sdev_frames=0;
}
int main(void){
	short *samples;
	char *d;
	int i;
	void *p=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
	assert(p!=MAP_FAILED&&(uintptr_t)p<0x80000000UL);
	d=p;
	samples=(short *)(d+128);
	descriptor=d;
	*(unsigned int *)(d+16)=4;
	*(unsigned int *)(d+20)=(uintptr_t)samples;
	*(unsigned short *)(d+8)=2;
	*(unsigned short *)(d+10)=16;
	reset();
	for(i=0;i<8;i++)samples[i]=300;
	pulls=1;
	assert(pull());
	memset(samples,0,16);
	pulls=1;
	assert(pull());
	for(i=0;i<8;i++)samples[i]=700;
	pulls=1;
	assert(pull());
	assert(head==48&&sdev_frames==12);
	for(i=0;i<8;i++)assert(((short *)ring)[i]==300&&((short *)ring)[8+i]==0&&((short *)ring)[16+i]==700);
	reset();
	tm_run();
	assert(primes==0&&deferred==0&&writes==0);
	running=tmon=1;
	pulls=0;
	tm_run();
	assert(!running&&!tmon&&primes==0&&deferred==0);
	reset();
	head=16;
	limit=2;
	flushout();
	assert(sfd==3&&hoff==6&&tail==0);
	limit=100000;
	flushout();
	assert(tail==head&&left==0&&hoff==hlen);
	reset();
	head=16;
	running=0;
	tmon=1;
	tm_run();
	assert(deferred==1&&primes==1);
	dt_run();
	tm_run();
	assert(!tmon&&head==tail);
	munmap(p,4096);
	puts("PASS component: audible/silent continuity, idle timer stop, partial writes, buffered drain");
	return 0;
}
