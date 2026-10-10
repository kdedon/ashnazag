#include "../message.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int count;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d\n",__LINE__); exit(1); } count++; } while(0)

int main(void)
{
	unsigned char ctl[8] = {0,0,0,101}, a[3] = {1,2,3}, b[2] = {4,5}, out[32];
	struct otr_datab blocks[3];
	struct otr_msg m[3];
	struct otr_view v;
	int n;

	memset(blocks,0,sizeof blocks); memset(m,0,sizeof m);
	blocks[0].base=ctl; blocks[0].limit=ctl+sizeof ctl; blocks[0].type=OTR_M_PROTO;
	blocks[1].base=a; blocks[1].limit=a+sizeof a;
	blocks[2].base=b; blocks[2].limit=b+sizeof b;
	m[0].block=&blocks[0]; m[0].read=ctl; m[0].write=ctl+4; m[0].cont=&m[1];
	m[1].block=&blocks[1]; m[1].read=a; m[1].write=a+3; m[1].cont=&m[2];
	m[2].block=&blocks[2]; m[2].read=b; m[2].write=b+2;
	n=otr_message(out,sizeof out,m); CHECK(n==17);
	CHECK(otr_view(out,n,&v)==OTR_DONE && v.type==OTB_R_PROTO && v.clen==4 && v.dlen==5);
	CHECK(!memcmp(v.ctl,ctl,4) && !memcmp(v.data,"\1\2\3\4\5",5));
	CHECK(m[0].read==ctl && m[1].read==a && m[2].read==b);
	blocks[0].type=OTR_M_PCPROTO; CHECK(otr_message(out,sizeof out,m)==17 && out[0]==OTB_R_PCPROTO);
	CHECK(otr_message(out,sizeof out,&m[1])==13 && out[0]==OTB_R_DATA);
	CHECK(otr_message(out,16,m)==OTR_BAD);
	m[0].write=ctl+3; CHECK(otr_message(out,sizeof out,m)==OTR_BAD); m[0].write=ctl+4;
	m[1].write=(unsigned char *)((unsigned long)a+4); CHECK(otr_message(out,sizeof out,m)==OTR_BAD); m[1].write=a+3;
	m[1].read=a+3; m[1].write=a; CHECK(otr_message(out,sizeof out,m)==OTR_BAD); m[1].read=a; m[1].write=a+3;
	blocks[1].type=OTR_M_PROTO; CHECK(otr_message(out,sizeof out,m)==OTR_BAD); blocks[1].type=OTR_M_DATA;
	m[1].band=1; CHECK(otr_message(out,sizeof out,m)==OTR_BAD); m[1].band=0;
	m[1].flags=4; CHECK(otr_message(out,sizeof out,m)==OTR_BAD); m[1].flags=0;
	m[2].cont=&m[1]; CHECK(otr_message(out,sizeof out,m)==OTR_BAD); m[2].cont=0;
	m[0].block=0; CHECK(otr_message(out,sizeof out,m)==OTR_BAD); m[0].block=&blocks[0];
	CHECK(otr_message(0,sizeof out,m)==OTR_BAD && otr_message(out,sizeof out,0)==OTR_BAD);
	printf("PASS %d message checks\n",count);
	return 0;
}
