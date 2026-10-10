#include "../bootstrap.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int count;
#define CHECK(c) do { count++; if (!(c)) { fprintf(stderr,"FAIL line %d\n",__LINE__); exit(1); } } while(0)
struct fixture {
	int closed, released, delivered, blocked, failwrite, failread, stage, fail;
	unsigned long written, dlen;
	unsigned char wire[OTR_BUFFER];
	unsigned char input[16][OTB_MAXREC];
	int length[16], cursor;
	struct otr_provider *p;
	int recurse, nested, closewrite;
};
static long rd(void *a, int fd, unsigned char *p, unsigned long n) {
	struct fixture *f=a; (void)fd;
	if(f->failread) return -1;
	if(f->cursor == 16 || !f->length[f->cursor]) return 0;
	CHECK((unsigned long)f->length[f->cursor]<=n);
	memcpy(p,f->input[f->cursor],f->length[f->cursor]);
	return f->length[f->cursor++];
}
static long wr(void *a, int fd, const unsigned char *p, unsigned long n) {
	struct fixture *f=a; (void)fd;
	if(f->closewrite) {otr_provider_close(f->p); return n;}
	if(f->failwrite) return -1;
	if(f->blocked) return 0;
	if(n>3) n=3;
	memcpy(f->wire+f->written,p,n); f->written+=n; return n;
}
static void cl(void *a,int fd) {struct fixture*f=a;(void)fd;f->closed++;}
static void rel(void*a,struct otr_msg*m) {struct fixture*f=a;(void)m;f->released++;}
static int deliver(void*a,void*q,int type,const unsigned char*c,unsigned long cn,
    const unsigned char*d,unsigned long dn) {
	struct fixture*f=a;(void)q;(void)c;
	if(f->recurse) f->nested=otr_provider_pump(f->p,1);
	if(f->blocked) return OTR_WAIT;
	CHECK(type==OTB_R_PROTO || type==OTB_R_DATA);
	CHECK(cn==(type==OTB_R_PROTO?4UL:0UL));
	CHECK(!dn || d[0]==7); f->delivered++;f->dlen=dn;return OTR_DONE;
}
static const struct otr_provider_ops ops={rd,wr,cl,rel,deliver};
static int ses(void*a) {struct fixture*f=a;return ++f->stage==f->fail?-1:10;}
static long sid(void*a,int fd) {struct fixture*f=a;(void)fd;return ++f->stage==f->fail?-1:23;}
static int end(void*a,int proto) {struct fixture*f=a;CHECK(proto==OTR_TCP||proto==OTR_UDP);return ++f->stage==f->fail?-1:11;}
static int push(void*a,int fd) {struct fixture*f=a;CHECK(fd==11);return ++f->stage==f->fail?-1:0;}
static int attach(void*a,int fd,unsigned long id,unsigned long cookie,int proto) {
	struct fixture*f=a;CHECK(fd==11&&id==23&&(cookie==17||cookie==18));CHECK(proto==OTR_TCP||proto==OTR_UDP);
	return ++f->stage==f->fail?-1:0;
}
static const struct otr_boot_ops boots={ses,sid,end,push,attach,cl};
static long put(void*q,struct otr_msg*m) {(void)q;(void)m;return 0;}
static long op(void*q,void*d,long f,long s,void*c) {(void)q;(void)d;(void)f;(void)s;(void)c;return 0;}
static long closeq(void*q,long f,void*c) {(void)q;(void)f;(void)c;return 0;}
int main(void) {
	struct otr_provider *p=calloc(1,sizeof(*p));
	struct fixture *f=calloc(1,sizeof(*f));
	struct otr_datab block;struct otr_msg m;
	struct otr_boot b, child, secondchild;struct otr_install install;
	struct otr_qinit r,w;struct otr_streamtab table;
	unsigned char data[]={7,8,9,10,11,12,13};unsigned char ctl[]={0,0,0,1};
	int i,n;
	CHECK(p&&f);memset(&block,0,sizeof(block));memset(&m,0,sizeof(m));
	block.base=data;block.limit=data+sizeof(data);block.type=OTR_M_DATA;
	m.block=&block;m.read=data;m.write=data+sizeof(data);
	CHECK(otr_provider_open(p,&ops,f,f,11)==OTR_DONE);
	CHECK(otr_provider_open(p,&ops,f,f,12)==OTR_BAD);
	CHECK(otr_provider_send(p,&m)==OTR_DONE);
	CHECK(otr_provider_send(p,&m)==OTR_WAIT);f->blocked=1;
	CHECK(otr_provider_pump(p,1)==OTR_WAIT&&f->released==0&&f->written==0);
	f->blocked=0;for(i=0;i<5;i++)CHECK(otr_provider_pump(p,1)>=0);
	CHECK(f->released==1&&f->written==15&&p->pending==0);
	CHECK(!memcmp(f->wire+8,data,sizeof(data))&&m.read==data);
	f->length[0]=otr_pack(f->input[0],OTB_MAXREC,OTB_R_PROTO,ctl,4,data,3);
	f->input[0][1]=OTB_RF_MORE;
	f->length[1]=otr_pack(f->input[1],OTB_MAXREC,OTB_R_PROTO,0,0,data+3,4);
	/* Continuation records retain their type and omit control bytes. */
	CHECK(f->length[1]==OTR_BAD);
	f->length[1]=12;memset(f->input[1],0,12);f->input[1][0]=OTB_R_PROTO;
	f->input[1][7]=4;memcpy(f->input[1]+8,data+3,4);
	f->blocked=1;CHECK(otr_provider_pump(p,3)==OTR_DONE);
	CHECK(p->complete&&f->delivered==0&&f->cursor==2);
	f->blocked=0;f->recurse=1;f->p=p;
	CHECK(otr_provider_pump(p,1)==OTR_DONE);
	CHECK(f->delivered==1&&f->dlen==7&&f->nested==OTR_WAIT);
	CHECK(otr_provider_send(p,&m)==OTR_DONE);f->failwrite=1;
	CHECK(otr_provider_pump(p,1)==OTR_BAD);
	CHECK(f->closed==1&&f->released==2&&!p->live);
	otr_provider_close(p);CHECK(f->closed==1);
	memset(f,0,sizeof(*f));CHECK(otr_provider_open(p,&ops,f,f,11)==1);
	f->length[0]=otr_pack(f->input[0],OTB_MAXREC,OTB_R_DATA,0,0,data,1);
	f->input[0][1]=OTB_RF_MORE;
	f->length[1]=otr_pack(f->input[1],OTB_MAXREC,OTB_R_PROTO,ctl,4,data,1);
	CHECK(otr_provider_pump(p,2)==OTR_BAD&&f->closed==1);
	memset(f,0,sizeof(*f));CHECK(otr_provider_open(p,&ops,f,f,11)==1);
	f->failread=1;CHECK(otr_provider_pump(p,1)==OTR_BAD&&f->closed==1);
	memset(f,0,sizeof(*f));CHECK(otr_provider_open(p,&ops,f,f,11)==1);
	m.band=1;CHECK(otr_provider_send(p,&m)==OTR_BAD&&f->released==0);m.band=0;
	for(i=0;i<9;i++) {
		unsigned long payload=i==0?8180UL:i==8?68UL:8184UL;
		unsigned char *rec=f->input[i];
		unsigned long j,cn=i==0?4:0;
		f->length[i]=(int)(8+cn+payload);rec[0]=OTB_R_PROTO;rec[1]=i==8?0:OTB_RF_MORE;
		rec[2]=0;rec[3]=(unsigned char)cn;rec[4]=rec[5]=0;
		rec[6]=(unsigned char)(payload>>8);rec[7]=(unsigned char)payload;
		for(j=0;j<cn;j++)rec[8+j]=ctl[j];
		for(j=0;j<payload;j++)rec[8+cn+j]=7;
	}
	CHECK(otr_provider_pump(p,12)==1&&f->delivered==1&&f->dlen==65536);
	otr_provider_close(p);CHECK(f->closed==1);
	memset(f,0,sizeof(*f));CHECK(otr_provider_open(p,&ops,f,f,11)==1);
	f->length[0]=otr_pack(f->input[0],OTB_MAXREC,OTB_R_DATA,0,0,data,1);
	f->input[0][1]=OTB_RF_MORE;
	f->length[1]=8;f->input[1][0]=OTB_R_DATA;f->input[1][1]=OTB_RF_MORE;
	CHECK(otr_provider_pump(p,2)==OTR_BAD&&f->closed==1);
	memset(f,0,sizeof(*f));CHECK(otr_provider_open(p,&ops,f,f,11)==1);
	f->length[0]=8;f->input[0][0]=OTB_R_DATA;f->input[0][1]=2;
	CHECK(otr_provider_pump(p,1)==OTR_BAD&&f->closed==1);
	memset(f,0,sizeof(*f));CHECK(otr_provider_open(p,&ops,f,f,11)==1);
	CHECK(otr_provider_send(p,&m)==1);f->closewrite=1;f->p=p;
	CHECK(otr_provider_pump(p,1)>=0&&f->closed==1&&f->released==1&&!p->live);
	for(i=1;i<=5;i++) {
		memset(&b,0,sizeof(b));memset(f,0,sizeof(*f));f->fail=i;
		CHECK(otr_boot_open(&b,&boots,f,OTR_TCP,17)==OTR_BAD);
		CHECK(!b.live&&b.fd==-1&&b.session==-1);
		n=i==1?0:i<=3?1:2;CHECK(f->closed==n);
		otr_boot_close(&b);CHECK(f->closed==n);
	}
	memset(&b,0,sizeof(b));memset(f,0,sizeof(*f));
	CHECK(otr_boot_open(&b,&boots,f,OTR_UDP,17)==OTR_DONE);
	CHECK(otr_boot_open(&b,&boots,f,OTR_UDP,17)==OTR_BAD);
	memset(&child,0,sizeof(child));f->stage=2;
	CHECK(otr_boot_join(&child,&b,OTR_UDP,17)==1&&b.children==1&&child.id==b.id);
	CHECK(otr_boot_close(&b)==OTR_WAIT&&b.live&&f->closed==0);
	CHECK(otr_boot_close(&child)==1&&b.children==0&&f->closed==1);
	CHECK(otr_boot_close(&b)==1&&f->closed==3);
	memset(&b,0,sizeof(b));memset(&child,0,sizeof(child));
	memset(&secondchild,0,sizeof(secondchild));memset(f,0,sizeof(*f));
	CHECK(otr_boot_session(&b,&boots,f)==1);
	CHECK(otr_boot_join(&child,&b,OTR_TCP,17)==1&&b.children==1);
	CHECK(otr_boot_join(&secondchild,&b,OTR_TCP,18)==1&&b.children==2);
	CHECK(otr_boot_close(&b)==OTR_WAIT&&b.live&&!f->closed);
	CHECK(otr_boot_close(&child)==1&&b.children==1&&secondchild.live&&b.live);
	CHECK(otr_boot_close(&b)==OTR_WAIT&&b.live&&f->closed==1);
	CHECK(otr_boot_close(&secondchild)==1&&b.children==0&&b.live);
	CHECK(otr_boot_close(&b)==1&&!b.live&&f->closed==3);
	memset(&b,0,sizeof(b));memset(&child,0,sizeof(child));
	memset(&secondchild,0,sizeof(secondchild));memset(f,0,sizeof(*f));
	CHECK(otr_boot_session(&b,&boots,f)==1);
	CHECK(otr_boot_join(&child,&b,OTR_TCP,17)==1);
	f->stage=0;f->fail=3;
	CHECK(otr_boot_join(&secondchild,&b,OTR_TCP,18)==OTR_BAD);
	CHECK(!secondchild.live&&child.live&&b.live&&b.children==1&&f->closed==1);
	CHECK(otr_boot_close(&b)==OTR_WAIT&&b.live&&f->closed==1);
	CHECK(otr_boot_close(&child)==1&&b.children==0&&f->closed==2);
	CHECK(otr_boot_close(&b)==1&&!b.live&&f->closed==3);
	memset(&b,0,sizeof(b));memset(f,0,sizeof(*f));
	CHECK(otr_boot_open(&b,&boots,f,OTR_TCP,17)==1);f->stage=2;f->fail=4;
	CHECK(otr_boot_join(&child,&b,OTR_TCP,17)==-1&&b.children==0&&f->closed==1);
	CHECK(otr_boot_close(&b)==1&&f->closed==3);
	memset(&r,0,sizeof(r));memset(&w,0,sizeof(w));memset(&table,0,sizeof(table));
	r.put=w.put=put;r.open=op;r.close=closeq;r.info=w.info=f;table.read=&r;table.write=&w;
	CHECK(otr_install_contract(&install,&table,OTR_TCP)==1);
	CHECK(install.flags==0x1001&&install.sync==3&&!install.buddy&&!install.loads&&!install.references);
	CHECK(otr_install_contract(&install,&table,OTR_UDP)==1&&install.flags==0x1001&&install.sync==1);
	w.put=0;CHECK(otr_install_contract(&install,&table,OTR_UDP)==-1);
	free(f);free(p);printf("PASS %d provider/bootstrap checks\n",count);return 0;
}
