#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "anchor.h"
static int checks;
#define CHECK(x) do {assert(x);checks++;}while(0)
static unsigned int g16(unsigned char*p){return p[0]*256+p[1];}
static void w16(unsigned char*p,int v){p[0]=v>>8;p[1]=v;}
static void checksum(unsigned char*p){unsigned long n=0;int j;p[10]=p[11]=0;for(j=0;j<20;j+=2)n+=g16(p+j);while(n>>16)n=(n&65535)+(n>>16);w16(p+10,~n);}
static unsigned char mac[6]={2,1,2,3,0,9};
static int packet(unsigned char*p,int msg){int n=14+20+8+244;memset(p,0,1514);memset(p,255,6);memcpy(p+6,mac,6);w16(p+12,0x800);p[14]=0x45;p[22]=64;p[23]=17;w16(p+16,n-14);memset(p+30,255,4);checksum(p+14);w16(p+34,68);w16(p+36,67);w16(p+38,n-34);p[42]=1;p[43]=1;p[44]=6;p[46]=0x12;p[47]=0x34;p[48]=0x56;p[49]=0x78;memcpy(p+70,mac,6);memcpy(p+278,"\143\202\123\143",4);p[282]=53;p[283]=1;p[284]=msg;p[285]=255;return n;}
int main(void){struct otb_identity i,bad;unsigned char p[1514],o[1514];int n,r,j;memset(&i,0,sizeof i);i.version[3]=1;memcpy(i.address,"\012\000\002\017",4);memcpy(i.mask,"\377\377\377\000",4);memcpy(i.router,"\012\000\002\002",4);memcpy(i.dns,"\012\000\002\003",4);strcpy((char*)i.domain,"example.test");CHECK(otb_identity_valid(&i));bad=i;bad.mask[2]=1;CHECK(!otb_identity_valid(&bad));bad=i;bad.address[0]=224;CHECK(!otb_identity_valid(&bad));bad=i;bad.router[2]=3;CHECK(!otb_identity_valid(&bad));bad=i;memset(bad.domain,'a',64);CHECK(!otb_identity_valid(&bad));
 n=packet(p,1);r=otb_anchor_reply(&i,mac,p,n,o,sizeof o);CHECK(r>0);CHECK(o[42]==2&&o[284]==2);CHECK(!memcmp(o+46,p+46,4));CHECK(!memcmp(o+58,i.address,4));CHECK(g16(o+16)==r-14&&g16(o+38)==r-34);CHECK(g16(o+34)==67&&g16(o+36)==68);CHECK(!memcmp(o+70,mac,6));CHECK(otb_anchor_reply(&i,mac,p,n,o,449)<0);
 n=packet(p,3);memcpy(p+54,i.address,4);CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)>0&&o[284]==5);
 p[285]=50;p[286]=4;memcpy(p+287,"\012\000\002\020",4);p[291]=255;n+=6;w16(p+16,n-14);w16(p+38,n-34);checksum(p+14);CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)>0&&o[284]==6);CHECK(!memcmp(o+58,"\0\0\0\0",4));
 n=packet(p,4);CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)==0);
 n=packet(p,8);memcpy(p+54,i.address,4);CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)>0&&o[284]==5);CHECK(!memcmp(o+58,"\0\0\0\0",4));
 n=packet(p,1);p[284]=3;p[285]=54;p[286]=4;memcpy(p+287,i.router,4);p[291]=255;n+=6;w16(p+16,n-14);w16(p+38,n-34);checksum(p+14);CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)==0);
 n=packet(p,1);p[14]=0x46;CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)<0);
 n=packet(p,1);p[20]=0x20;CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)<0);
 n=packet(p,1);p[283]=30;CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)<0);
 n=packet(p,1);p[70]^=1;CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)<0);
 n=packet(p,1);p[6]^=1;CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)<0);
 n=packet(p,1);p[35]=69;CHECK(otb_anchor_reply(&i,mac,p,n,o,sizeof o)==0);
 memset(p,0,1514);memcpy(p+6,mac,6);w16(p+12,0x806);w16(p+14,1);w16(p+16,0x800);p[18]=6;p[19]=4;w16(p+20,1);memcpy(p+22,mac,6);memcpy(p+28,i.address,4);memcpy(p+38,i.router,4);CHECK(otb_anchor_reply(&i,mac,p,42,o,sizeof o)==42);CHECK(g16(o+20)==2&&!memcmp(o+28,i.router,4)&&!memcmp(o+38,i.address,4));memcpy(p+38,i.address,4);CHECK(otb_anchor_reply(&i,mac,p,42,o,sizeof o)==0);
 for(j=0;j<1514;j++){p[j]=(j*13)^47;otb_anchor_reply(&i,mac,p,j,o,sizeof o);}CHECK(1);
 printf("PASS %d anchor checks\n",checks);return 0;}
