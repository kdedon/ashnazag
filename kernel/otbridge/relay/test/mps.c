#define _GNU_SOURCE
#include <sys/mman.h>
#include "../mps.h"
#include "../host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long tword(const unsigned char *p){return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];}
static void tput(unsigned char *p,unsigned long n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static int checks, states, messages, sessions, endpoints, closed, enabled, arms;
static int blocked, failopen, eof, notifytype, failmsg;
static unsigned char upstream[OTB_MAXREC], wire[OTR_BUFFER];
static unsigned long upsize, written, fragment;
static struct otr_msg *received, *lasttimer;
static int waits, delayedreply, waitclose;
static int callbackclose, canceledqueued, autoreply, fastframe, fastdeliver, failwrite;
static void *validatedtimer;
static unsigned long last_cookie,checkedtoken;
static struct otr_msg *checkedhead;
static void *closeread;
static struct otr_qinit *closeops;

struct queue {
	struct otr_queue_prefix prefix;
	struct queue *mate;
};
static struct queue rq,wq,rq2,wq2;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL MPS line %d\n",__LINE__);exit(1); } } while(0)
static void close_callback(void) {
	callbackclose=0;CHECK(closeops->close(closeread,0,0)==0);
}
#ifdef OTR_TRACE_TPI
static unsigned long tracecounts[3], lastprimitive;
long otr_sys_write(int fd,const unsigned char *line,unsigned long n) {
    unsigned long identity,phase,type,primitive;
    char copy[81];
    CHECK(fd==1 && n<sizeof(copy) && line[n-1]=='\n');
    memcpy(copy,line,n);copy[n]=0;
    CHECK(sscanf(copy,"otrelay I tpi cookie/phase/type/primitive %lx %lx %lx %lx",
        &identity,&phase,&type,&primitive)==4 && identity && phase<3);
    tracecounts[phase]++;lastprimitive=primitive;
    return n;
}
#endif
void *OTAllocMem(unsigned long n) {void*p=calloc(1,n);if(p)states++;return p;}
void OTFreeMem(void*p) {CHECK(p&&states>0);states--;free(p);}
struct otr_msg *allocb(unsigned long n,long priority) {
	struct otr_msg*m;struct otr_datab*d;
	if(failmsg && !--failmsg)return 0;
	m=calloc(1,sizeof(*m));d=calloc(1,sizeof(*d));
	(void)priority;CHECK(m&&d);d->base=malloc(n?n:1);CHECK(d->base);
	d->limit=d->base+n;m->read=m->write=d->base;m->block=d;messages++;return m;
}
void freeb(struct otr_msg*m) {CHECK(m&&messages>0);if(m==checkedhead){CHECK(tword(m->read+4)==checkedtoken);checkedhead=0;}messages--;free(m->block->base);free(m->block);free(m);}
void freemsg(struct otr_msg*m) {while(m){struct otr_msg*n=m->cont;freeb(m);m=n;}}
int canputnext(void*q) {(void)q;return !blocked;}
static int handoffmutate;
int putnext(void*q,struct otr_msg*m) {if(fastframe)fastdeliver++;if(handoffmutate){struct otr_queue_prefix *up=((struct otr_queue_prefix*)q)->next;((unsigned char*)up->link)[32]=1;up->first=up->last=m;handoffmutate=0;}if(callbackclose==2){freemsg(m);close_callback();return 1;}CHECK(!received);received=m;return 1;}
void *WR(void*q) {return ((struct queue*)q)->mate;}
void *RD(void*q) {return ((struct queue*)q)->mate;}
int putq(void*q,struct otr_msg*m) {
	struct otr_queue_prefix*p=q;m->next=0;
	if(p->last)p->last->next=m;else p->first=m;p->last=m;return 1;
}
struct otr_msg *getq(void*q) {
	struct otr_queue_prefix*p=q;struct otr_msg*m=p->first;
	if(m){p->first=m->next;if(!p->first)p->last=0;m->next=0;}return m;
}
int putbq(void*q,struct otr_msg*m) {struct otr_queue_prefix*p=q;m->next=p->first;p->first=m;if(!p->last)p->last=m;return 1;}
void qenable(void*q) {(void)q;enabled++;if(callbackclose==1)close_callback();}
void qprocson(void*q) {(void)q;if(callbackclose==4)close_callback();}
void qprocsoff(void*q) {(void)q;}
void flushq(void*q,long type) {struct otr_msg*m;(void)type;while((m=getq(q))!=0)freemsg(m);}
int putnextctl(void*q,long type) {(void)q;notifytype=type;return 1;}
int putnextctl1(void*q,long type,long value) {(void)q;CHECK(value==5);notifytype=type;if(callbackclose==3)close_callback();return 1;}
int qreply(void*q,struct otr_msg*m) {(void)q;if(callbackclose==5){freemsg(m);close_callback();return 1;}CHECK(!received);received=m;return 1;}
struct otr_msg *mi_timer_alloc(void*q,unsigned long n) {(void)q;(void)n;lasttimer=allocb(0,2);return lasttimer;}
void mi_timer(void*p,unsigned long ms) {CHECK(p&&ms>=8&&ms<=64);arms++;}
static int queued(void *p) {
	struct otr_msg *m;
	for(m=rq.prefix.first;m;m=m->next)if(m==p)return 1;
	for(m=rq2.prefix.first;m;m=m->next)if(m==p)return 1;
	return 0;
}
int mi_timer_cancel(void*p) {CHECK(p);canceledqueued=queued(p);validatedtimer=0;return 1;}
void mi_timer_free(void*p) {CHECK(!queued(p)&&(!canceledqueued||validatedtimer==p));canceledqueued=0;freeb(p);}
int mi_timer_valid(void*p) {CHECK(p&&!queued(p));validatedtimer=p;return !canceledqueued;}
static int sess(void*a) {(void)a;sessions++;return 10;}
static long sid(void*a,int fd) {(void)a;CHECK(fd==10);return 23;}
static int end(void*a,int proto) {(void)a;CHECK(proto==OTR_TCP||proto==OTR_UDP);if(failopen)return -1;endpoints++;return 10+endpoints;}
static int push(void*a,int fd) {(void)a;CHECK(fd>10);return 0;}
static int attach(void*a,int fd,unsigned long id,unsigned long cookie,int proto) {
	(void)a;(void)proto;CHECK(fd>10&&id==23&&cookie);last_cookie=cookie;return 0;
}
static void closefd(void*a,int fd) {(void)a;CHECK(fd>=10);closed++;}
long otr_host_step, otr_host_result;
const struct otr_boot_ops otr_host_boot={sess,sid,end,push,attach,closefd};
long otr_host_read(void*a,int fd,unsigned char*p,unsigned long n) {
	(void)a;CHECK(fd>10);if(eof)return OTR_HOST_EOF;
	if(!upsize)return 0;CHECK(upsize<=n);memcpy(p,upstream,upsize);n=upsize;upsize=0;return n;
}
long otr_host_write(void*a,int fd,const unsigned char*p,unsigned long n) {
	(void)a;CHECK(fd>10);if(failwrite)return -1;if(fragment&&n>fragment)n=fragment;
	CHECK(written+n<=sizeof(wire));memcpy(wire+written,p,n);written+=n;
    if(autoreply && written==12 && wire[11]==107) {
        unsigned char ack[44]={0};ack[3]=129;ack[23]=16;ack[35]=2;
        upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PCPROTO,ack,sizeof(ack),0,0);
    }
    return n;
}
int otr_host_wait(int fd,int events,unsigned long ms) {
    CHECK(fd>10&&(events==1||events==2)&&ms==100);waits++;
    if(waitclose) {waitclose=0;close_callback();return 1;}
    if(delayedreply && waits==delayedreply) {
        unsigned char ack[44]={0};ack[3]=129;ack[35]=2;
        upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PCPROTO,ack,sizeof(ack),0,0);
    }
    return 0;
}
int otr_host_flush(int fd,int flags) {CHECK(fd>10&&(flags&~3)==0);written=0;return 0;}
void otr_host_close(void*a,int fd) {closefd(a,fd);}
static void pair(struct queue*r,struct queue*w) {memset(r,0,sizeof(*r));memset(w,0,sizeof(*w));r->mate=w;w->mate=r;}
static struct otr_msg *data(unsigned long n) {struct otr_msg*m=allocb(n,2);m->block->type=OTR_M_DATA;memset(m->write,7,n);m->write+=n;return m;}
static struct otr_msg *acceptrequest(unsigned long token) {
    struct otr_msg *m=allocb(20,2);m->block->type=OTR_M_PROTO;
    memset(m->read,0,20);tput(m->read,103);tput(m->read+4,token);tput(m->read+16,0x12345678UL);m->write+=20;return m;
}
static struct otr_msg *inforequest(void) {
    struct otr_msg *m=allocb(4,2);m->block->type=OTR_M_PCPROTO;
    memset(m->write,0,4);m->write[3]=107;m->write+=4;return m;
}
int main(void) {
	struct otr_install *tcp=GetOTauxtcpInstallInfo(),*udp=GetOTauxudpInstallInfo();
	struct otr_msg*m;unsigned char d[]={7,8,9};unsigned char c[]={0,0,0,129};
	int before;
    unsigned long device=0x56780000UL;
	CHECK(states==0&&messages==0&&sessions==0);
	CHECK(tcp->flags==0x1001&&tcp->sync==4&&udp->flags==0x1001&&udp->sync==4);
	pair(&rq,&wq);pair(&rq2,&wq2);
    {
        unsigned long first=0x12340000UL, second=0x12340000UL, saved;
        CHECK(tcp->table->read->open(&rq,0,0,2,0)==22 && !states);
        CHECK(tcp->table->read->open(&rq,&first,0,2,0)==0);
        CHECK(first==0x1234000aUL);
        CHECK(tcp->table->read->open(&rq2,&second,0,2,0)==0);
        CHECK(second==0x1234000bUL && first!=second);
        saved=first;first=0x12340000UL;
        CHECK(tcp->table->read->open(&rq,&first,0,2,0)==0 && first==saved);
        CHECK(tcp->table->read->close(&rq,0,0)==0);
        first=0x12340000UL;
        CHECK(tcp->table->read->open(&rq,&first,0,2,0)==0 && first==saved);
        CHECK(tcp->table->read->close(&rq,0,0)==0);
        CHECK(tcp->table->read->close(&rq2,0,0)==0);
        CHECK(!states && !messages);
        failopen=1;first=0xabcd0000UL;
        CHECK(tcp->table->read->open(&rq,&first,0,2,0)!=0 && first==0xabcd0000UL);
        failopen=0;closeread=&rq;closeops=tcp->table->read;callbackclose=4;
        CHECK(closeops->open(&rq,&first,0,2,0)==22 && first==0xabcd0000UL);
        CHECK(closeops->open(&rq,&first,0,2,0)==0 && first==0xabcd000aUL);
        CHECK(closeops->close(&rq,0,0)==0 && !states && !messages);
        sessions=endpoints=closed=0;
    }

	CHECK(tcp->table->read->open(&rq,0,0,0,0)==22&&states==0);
	CHECK(tcp->table->read->open(&rq,0,0,3,0)==22&&states==0);
	CHECK(tcp->table->read->open(&rq,0,0,1,0)==0);
	CHECK(states==1&&messages==1&&sessions==1&&endpoints==1);
	CHECK(rq.prefix.private==wq.prefix.private&&rq.prefix.private);
	CHECK(tcp->table->read->open(&rq,0,0,1,0)==0&&states==1);
	CHECK(udp->table->read->open(&rq2,0,0,1,0)==0);
	CHECK(states==2&&sessions==1&&endpoints==2);
	m=data(7);tcp->table->write->put(&wq,m);tcp->table->read->service(&rq);
	CHECK(written==15&&messages==2&&!memcmp(wire+8,"\7\7\7\7\7\7\7",7));
	upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PROTO,c,4,d,3);blocked=1;
	tcp->table->read->service(&rq);CHECK(!received);
	blocked=0;tcp->table->read->service(&rq);
	CHECK(received&&received->block->type==OTR_M_PROTO&&received->write-received->read==4);
	CHECK(received->cont&&received->cont->write-received->cont->read==3);
	freemsg(received);received=0;
	upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PROTO,c,4,d,3);
	failmsg=2;tcp->table->read->service(&rq);CHECK(!received&&messages==2);
	tcp->table->read->service(&rq);CHECK(received&&received->cont);
	freemsg(received);received=0;
	fragment=3;written=0;tcp->table->write->put(&wq,data(30));tcp->table->read->service(&rq);
	CHECK(written==3);m=data(1);m->block->type=0x86;m->read[0]=2;
	tcp->table->write->put(&wq,m);CHECK(written==0&&messages==2);fragment=0;
	tcp->table->write->put(&wq,data(2));tcp->table->read->service(&rq);CHECK(written==10);
	CHECK(tcp->table->read->close(&rq,0,0)==0&&states==1&&closed==1&&sessions==1);
	CHECK(!rq.prefix.private&&!wq.prefix.private);
	eof=1;udp->table->read->service(&rq2);CHECK(notifytype==0x89&&closed==3);eof=0;
	CHECK(udp->table->read->close(&rq2,0,0)==0&&states==0&&messages==0);
	before=closed;failopen=1;
	CHECK(tcp->table->read->open(&rq,0,0,1,0)!=0);
	CHECK(!states&&!messages&&closed==before+1&&!rq.prefix.private);failopen=0;
	CHECK(tcp->table->read->open(&rq,0,0,1,0)==0);
	m=data(1);m->block->type=0x55;tcp->table->write->put(&wq,m);CHECK(notifytype==0x8a);
	CHECK(tcp->table->read->close(&rq,0,0)==0&&states==0&&messages==0);
	closeread=&rq;closeops=tcp->table->read;
	CHECK(closeops->open(&rq,0,0,1,0)==0);callbackclose=1;
	tcp->table->write->put(&wq,data(2));CHECK(states==0&&messages==0&&!rq.prefix.private);
	CHECK(closeops->open(&rq,0,0,1,0)==0);
	upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PROTO,c,4,d,3);callbackclose=2;
	closeops->service(&rq);CHECK(states==0&&messages==0&&!rq.prefix.private);
	CHECK(closeops->open(&rq,0,0,1,0)==0);callbackclose=3;
	m=data(1);m->block->type=0x55;tcp->table->write->put(&wq,m);
	CHECK(states==0&&messages==0&&!rq.prefix.private);
	callbackclose=4;CHECK(closeops->open(&rq,0,0,1,0)==22);
	CHECK(states==0&&messages==0&&!rq.prefix.private);
	CHECK(closeops->open(&rq,0,0,1,0)==0);callbackclose=1;
	closeops->put(&rq,lasttimer);CHECK(states==0&&messages==0&&!rq.prefix.private);
	CHECK(closeops->open(&rq,0,0,1,0)==0);
	CHECK(putq(&rq,lasttimer));CHECK(putq(&rq,data(3)));
	CHECK(closeops->close(&rq,0,0)==0);
	CHECK(states==0&&messages==0&&!rq.prefix.first&&!rq.prefix.private);
    CHECK(closeops->open(&rq,&device,0,2,0)==0);
    CHECK(states==1&&rq.prefix.private);
#ifdef OTR_DIAGNOSTICS
    { unsigned long *words=(unsigned long *)((unsigned char *)GetOTauxtcpInstallInfo()+sizeof(struct otr_install));
      { struct { long command;void *credentials;unsigned long id;long count,error,result; } *io;
        m=data(sizeof(*io));m->block->type=0x0e;io=(void *)m->read;
        io->command=0x12345678L;io->count=37;
        tcp->table->write->put(&wq,m);
        CHECK(words[14]==0x12345678UL&&words[15]==37);
        CHECK(received==m&&m->block->type==0x82&&io->error==22);
        freemsg(received);received=0; }
      m=data(8);m->block->type=OTR_M_PROTO;
      memset(m->read,0,8);m->read[3]=103;m->read[4]=0xde;m->read[5]=0xad;m->read[6]=0xbe;m->read[7]=0xef;
      closeops=tcp->table->read;tcp->table->write->put(&wq,m);
      CHECK(words[14]==0xdeadbeefUL&&words[15]==(unsigned long)&rq);
#ifdef OTR_TRACE_TPI
      CHECK(words[224]==0x4f544143UL&&words[225]==1&&words[226]==0xdeadbeefUL);
      CHECK(words[227]!=0&&words[228]==(unsigned long)&rq&&words[229]==(unsigned long)&wq);
      CHECK(words[230]==1&&!words[231]);
      CHECK(words[232]==(unsigned long)&rq&&words[233]==(unsigned long)&wq);
#endif
      CHECK(rq.prefix.private&&states==1); }
#endif
    CHECK(closeops->close(&rq,0,0)==0);
    CHECK(states==0&&messages==0&&!rq.prefix.private);
#ifdef OTR_DIAGNOSTICS
    { unsigned long *words=(unsigned long *)((unsigned char *)GetOTauxtcpInstallInfo()+sizeof(struct otr_install));
      CHECK(words[0]==0x4f544447UL && words[1]==1 && words[2]==OTR_TCP);
      CHECK(words[3]>0 && words[4]==2 && words[6]>=1 && words[6]<=10); }
#endif
#ifdef OTR_TRACE_TPI
    { unsigned long before=tracecounts[0];int i;
      unsigned long *words=(unsigned long *)((unsigned char *)GetOTauxtcpInstallInfo()+sizeof(struct otr_install));
      unsigned char info[44]={0},code[2112]={0},head[112],link[48],*poll;
      struct otr_queue_prefix nq={0};struct otr_qinit ni={0};
      static const unsigned long sig[8]={0x4e56ffdc,0x48e71c38,0x206e0008,0x20280014,0x2d40ffdc,0x246e000c,0x206a0014,0x1428000d};
      for(i=0;i<8;i++){code[i*4]=sig[i]>>24;code[i*4+1]=sig[i]>>16;code[i*4+2]=sig[i]>>8;code[i*4+3]=sig[i];}
      for(i=0;i<112;i++)head[i]=i;
      memset(head+36,0,4);
      for(i=0;i<48;i++)link[i]=i+1;
      nq.link=link;
      ni.put=(long (*)(void *,struct otr_msg *))code;nq.info=&ni;nq.private=head;
      rq.prefix.next=&nq;
      CHECK(words[16]==0x4f545450UL && words[17]==0x000a00e0UL);
      for(i=18;i<224;i++)words[i]=0;
      written=0;fragment=0;blocked=0;
      CHECK(closeops->open(&rq,&device,0,2,0)==0);
      for(i=0;i<20;i++) {
        m=data(4);m->block->type=OTR_M_PROTO;memset(m->read,0,4);m->read[3]=OT_INFO_REQ;
        tcp->table->write->put(&wq,m);
      }
      CHECK(tracecounts[0]-before==8 && lastprimitive==OT_INFO_REQ);
      CHECK(words[18]==20 && words[21]==OT_INFO_REQ && words[22]==OT_INFO_REQ);
      for(i=1;i<11;i++)info[i*4+3]=i;
      info[3]=OT_INFO_ACK;upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PCPROTO,info,44,0,0);
      handoffmutate=1;closeops->service(&rq);
      CHECK(words[19]>0 && words[20]==1 && words[23]==OT_INFO_ACK && words[24]==OT_INFO_ACK);
      CHECK(words[26]==OTR_M_PROTO && words[27]==OTR_M_PCPROTO);
      CHECK(words[28]==OT_INFO_ACK);
      for(i=1;i<11;i++)CHECK(words[28+i]==(unsigned long)i);
      CHECK(words[39]==(unsigned long)&rq && words[40]==(unsigned long)rq.prefix.next);
      CHECK(words[41]==1 && words[42]==1 && words[43]==0);
      CHECK(words[44]==1 && words[50]==(unsigned long)head && words[51]==(unsigned long)code);
      CHECK(words[120]==1 && words[121]==(unsigned long)link);
      CHECK(words[134]==1 && words[135]==(unsigned long)&nq && words[136]==(unsigned long)link);
      for(i=0;i<12;i++)CHECK(words[137+i]==(i==8?0x01222324UL:words[122+i]) && words[122+i]==(((unsigned long)i*4+1)<<24|((unsigned long)i*4+2)<<16|((unsigned long)i*4+3)<<8|((unsigned long)i*4+4)));
      CHECK(words[149]==(unsigned long)received && words[150]==(unsigned long)received && words[151]==0x0c0d0e0fUL && words[152]==0 && words[159]==0x64656667UL);

      for(i=0;i<8;i++)CHECK(words[52+i]==sig[i]);
      for(i=0;i<28;i++)CHECK(words[60+i]==((unsigned long)head[i*4]<<24| (unsigned long)head[i*4+1]<<16 | (unsigned long)head[i*4+2]<<8 | head[i*4+3]));
      nq.first=nq.last=0;link[32]=33;
      CHECK(received && received->block->type==OTR_M_PCPROTO);
      freemsg(received);received=0;
      for(i=0;i<3;i++) {
        if(i==0)code[31]^=1;else if(i==1)nq.private=0;else nq.info=0;
        upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PCPROTO,info,44,0,0);closeops->service(&rq);
        CHECK(words[44]==0 && words[50]==0 && words[60]==0);
        CHECK(received!=0);freemsg(received);received=0;
        code[31]=(unsigned char)sig[7];nq.private=head;nq.info=&ni;
      }
      upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PCPROTO,info,44,0,0);closeops->service(&rq);
      CHECK(words[44]==1);CHECK(received!=0);freemsg(received);received=0;
      poll=mmap(0,32768,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
      CHECK(poll!=MAP_FAILED && (unsigned long)poll<=0xffffffffUL);
      {unsigned long addr=(unsigned long)poll;
       head[36]=addr>>24;head[37]=addr>>16;head[38]=addr>>8;head[39]=addr;}
      for(i=0;i<4;i++) {
        unsigned long next=(unsigned long)(poll+((i+1)%4)*24);
        poll[i*24]=next>>24;poll[i*24+1]=next>>16;poll[i*24+2]=next>>8;poll[i*24+3]=next;
        poll[i*24+11]=i+1;poll[i*24+21]=i+7;
      }
      upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PCPROTO,info,44,0,0);closeops->service(&rq);
      for(i=0;i<4;i++){CHECK(words[88+i*8]==(unsigned long)(poll+i*24));CHECK(words[91+i*8]==(unsigned long)i+1);CHECK(words[94+i*8]==((unsigned long)i+7)<<16);CHECK(words[95+i*8]==1);}
      CHECK(words[152]==(unsigned long)poll && words[155]==1 && words[158]==7UL<<16);
      CHECK(received!=0);freemsg(received);received=0;
      { unsigned char *ctx=poll+4096-26,*cell=poll+164,*queue=poll+168,*sync=poll+192;
        unsigned char *slots[4]={poll+12,ctx,cell,queue+16};
        unsigned char *values[4]={ctx,cell,queue,sync};
        for(i=0;i<4;i++) {unsigned long a=(unsigned long)values[i];slots[i][0]=a>>24;slots[i][1]=a>>16;slots[i][2]=a>>8;slots[i][3]=a;}
        CHECK(mprotect(poll+4096,4096,PROT_NONE)==0);
        { unsigned char *producer=code+0x80e,*slot=poll+300,*globals=poll+512,*callback=poll+8192;
          unsigned long addresses[2]={(unsigned long)slot,(unsigned long)globals};
          unsigned char *targets[2]={producer+4,slot};int j;
          producer[0]=0x2f;producer[1]=8;producer[2]=0x20;producer[3]=0x79;
          producer[8]=0x41;producer[9]=0xe8;producer[10]=1;producer[11]=0xfc;
          producer[12]=0x2f;producer[13]=8;producer[14]=0x4e;producer[15]=0xb9;
          for(j=0;j<2;j++){unsigned long a=addresses[j];targets[j][0]=a>>24;targets[j][1]=a>>16;targets[j][2]=a>>8;targets[j][3]=a;}
          { unsigned long a=(unsigned long)callback;globals[312]=a>>24;globals[313]=a>>16;globals[314]=a>>8;globals[315]=a; }
          globals[159]=7;globals[343]=3;globals[351]=1;globals[507]=9;globals[511]=11;
          { unsigned char *stub=poll+22000,*binding=poll+23000,*delegate=poll+24000,*descriptor=poll+25000;
            unsigned char *to[5]={stub+2,binding,delegate+2,globals+404,callback+0x82};
            unsigned char *from[5]={binding,delegate,descriptor,descriptor,stub};
            callback[0]=0x4e;callback[1]=0x56;callback[4]=0x2f;callback[5]=0x0a;callback[6]=0x70;
            callback[8]=0x20;callback[9]=0x79;callback[14]=0x41;callback[15]=0xe8;callback[16]=1;callback[17]=0x5e;
            callback[0x78]=0x42;callback[0x79]=0xa7;callback[0x7a]=0x48;callback[0x7b]=0x79;
            callback[0x80]=0x4e;callback[0x81]=0xb9;callback[0x86]=0x50;callback[0x87]=0x4f;
            stub[0]=0x41;stub[1]=0xf9;delegate[0]=0x41;delegate[1]=0xf9;
            delegate[6]=0x4a;delegate[7]=0x90;delegate[8]=0x66;delegate[9]=2;
            delegate[10]=0x4e;delegate[11]=0x75;delegate[12]=0x4a;delegate[13]=0xa8;
            delegate[15]=8;delegate[16]=0x67;delegate[17]=0x18;
            for(j=0;j<5;j++){unsigned long a=(unsigned long)from[j];to[j][0]=a>>24;to[j][1]=a>>16;to[j][2]=a>>8;to[j][3]=a;}
            descriptor[7]=0x40;
          }
        }
        ctx[25]=4;sync[32]=1;
        upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PCPROTO,info,44,0,0);closeops->service(&rq);
        CHECK(words[160]==(unsigned long)ctx && words[161]==1 && words[174]==4);
        CHECK(words[170]==(unsigned long)cell && words[171]==(unsigned long)queue);
        CHECK(words[172]==(unsigned long)sync && words[173]==1);
        CHECK(received!=0);freemsg(received);received=0;
        CHECK(words[168]==4UL<<16 && words[169]==0);
        CHECK(words[176]==1 && words[179]==(unsigned long)(poll+512));
        CHECK(words[180]==7 && words[184]==3 && words[186]==1);
        CHECK(words[188]==9 && words[189]==11 && words[190]==0x4e560000UL);
        CHECK(words[200]==1 && words[203]==(unsigned long)(poll+24000));
        CHECK(words[194]>>16==0x41f9);
        CHECK(words[201]==(unsigned long)(poll+22000) && words[201]!=(unsigned long)(poll+8192+0x23464));
        CHECK(words[204]==(unsigned long)(poll+25000) && words[205]==0 && words[206]==0x40);
        CHECK(words[218]==words[204] && words[219]==0 && words[220]==0x40);
        ctx[25]=0;sync[32]=0;
        upsize=otr_pack(upstream,sizeof(upstream),OTB_R_PCPROTO,info,44,0,0);closeops->service(&rq);
        CHECK(words[174]==0 && words[173]==0);
        CHECK(received!=0);freemsg(received);received=0;
        memset(poll+12,0,4);
      }
      memset(head+36,0,4);CHECK(munmap(poll,32768)==0);
      words[18]=words[19]=0xffffffffUL;
      m=data(4);m->block->type=OTR_M_PROTO;memset(m->read,0,4);m->read[3]=108;
      tcp->table->write->put(&wq,m);closeops->service(&rq);
      CHECK(words[18]==0xffffffffUL && words[19]==0xffffffffUL && words[21]==OT_INFO_REQ && words[22]==108);
      CHECK(closeops->close(&rq,0,0)==0);
      CHECK(words[43]==1 && words[39]==0 && words[40]==0);
      CHECK(words[44]==1 && words[60]==0x00010203UL);rq.prefix.next=0;
      CHECK(states==0&&messages==0&&!rq.prefix.private); }
#endif
    pair(&rq,&wq);closeread=&rq;closeops=tcp->table->read;
    CHECK(closeops->open(&rq,&device,0,2,0)==0);
    autoreply=1;fastframe=1;fastdeliver=0;written=0;
    tcp->table->write->put(&wq,inforequest());fastframe=0;
    CHECK(fastdeliver==1 && received && received->block->type==OTR_M_PCPROTO);
    CHECK(received->write-received->read==44 && received->read[3]==129);
    CHECK(!wq.prefix.first && !rq.prefix.first && written==12);
    freemsg(received);received=0;
    written=0;fragment=3;fastframe=1;fastdeliver=0;
    tcp->table->write->put(&wq,inforequest());fastframe=0;
    CHECK(fastdeliver==1 && written==12 && received);
    freemsg(received);received=0;fragment=0;
    written=0;autoreply=0;delayedreply=3;waits=0;fastframe=1;fastdeliver=0;
    tcp->table->write->put(&wq,inforequest());fastframe=0;
    CHECK(waits==3&&fastdeliver==1&&received&&received->read[3]==129);
    freemsg(received);received=0;delayedreply=0;
    written=0;autoreply=0;
    tcp->table->write->put(&wq,data(2));
    CHECK(written==0 && wq.prefix.first);
    closeops->service(&rq);CHECK(written==10);
    CHECK(closeops->close(&rq,0,0)==0 && !states && !messages);
    CHECK(closeops->open(&rq,&device,0,2,0)==0);
    written=0;autoreply=1;callbackclose=2;
    tcp->table->write->put(&wq,inforequest());
    CHECK(!states && !messages && !rq.prefix.private && !received);
    CHECK(closeops->open(&rq,&device,0,2,0)==0);
    written=0;autoreply=0;eof=1;notifytype=0;
    tcp->table->write->put(&wq,inforequest());eof=0;
    CHECK(notifytype==0x89);
    CHECK(closeops->close(&rq,0,0)==0 && !states && !messages);
    CHECK(closeops->open(&rq,&device,0,2,0)==0);
    written=0;failwrite=1;notifytype=0;
    tcp->table->write->put(&wq,inforequest());failwrite=0;
    CHECK(notifytype==0x8a && !received);
    CHECK(closeops->close(&rq,0,0)==0 && !states && !messages);
    CHECK(closeops->open(&rq,&device,0,2,0)==0);
    written=0;waits=0;notifytype=0;
    tcp->table->write->put(&wq,inforequest());
    CHECK(waits==8&&notifytype==0x8a&&!received);
    CHECK(closeops->close(&rq,0,0)==0&&!states&&!messages);
    CHECK(closeops->open(&rq,&device,0,2,0)==0);
    written=0;waits=0;waitclose=1;callbackclose=1;
    tcp->table->write->put(&wq,inforequest());
    CHECK(waits==1&&!states&&!messages&&!rq.prefix.private);
    {
        struct queue *a=mmap(0,4*sizeof(*a),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
        unsigned long ac,bc;int j;
        CHECK(a!=MAP_FAILED && (unsigned long)a<0xffffffffUL);pair(a,a+1);pair(a+2,a+3);
        CHECK(closeops->open(a,0,0,1,0)==0);ac=last_cookie;
        CHECK(closeops->open(a+2,0,0,1,0)==0);bc=last_cookie;
        for(j=0;j<2;j++) {
            written=0;m=acceptrequest((unsigned long)(a+j*2));
            checkedhead=m;checkedtoken=(unsigned long)(a+j*2);m->cont=data(3);
            tcp->table->write->put(a+1,m);CHECK(!checkedhead);
            closeops->service(a);
            CHECK(written==31 && tword(wire+8)==103 && tword(wire+12)==(j?bc:ac));
            CHECK(tword(wire+24)==0x12345678UL && wire[28]==7 && wire[30]==7);
            CHECK(states==2 && messages==2);
        }
        written=0;tcp->table->write->put(a+1,acceptrequest(1));
        CHECK(received && received->block->type==OTR_M_PCPROTO && tword(received->read)==127);
        CHECK(tword(received->read+4)==103 && tword(received->read+8)==4 && !written);
        freemsg(received);received=0;
        CHECK(closeops->close(a+2,0,0)==0);tcp->table->write->put(a+1,acceptrequest((unsigned long)(a+2)));
        CHECK(received && tword(received->read+8)==4);freemsg(received);received=0;
        CHECK(udp->table->read->open(a+2,0,0,1,0)==0);
        tcp->table->write->put(a+1,acceptrequest((unsigned long)(a+2)));
        CHECK(received && tword(received->read+8)==4);freemsg(received);received=0;
        CHECK(udp->table->read->close(a+2,0,0)==0);
        m=acceptrequest((unsigned long)a);failmsg=1;tcp->table->write->put(a+1,m);
        CHECK(received && tword(received->read+8)==8 && tword(received->read+12)==12);
        freemsg(received);received=0;failmsg=0;
        written=0;tcp->table->write->put(a+1,acceptrequest((unsigned long)a));closeops->service(a);
        CHECK(written==28 && tword(wire+12)==ac && states==1);
        closeread=a;callbackclose=5;tcp->table->write->put(a+1,acceptrequest(1));
        CHECK(!states && !messages && !a->prefix.private);
        CHECK(munmap(a,4*sizeof(*a))==0);
    }
	printf("PASS %d MPS provider checks\n",checks);return 0;
}
