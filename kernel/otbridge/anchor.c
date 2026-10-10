#include "anchor.h"
static unsigned long get32(p) unsigned char *p;
{ return (unsigned long)p[0]<<24 | (unsigned long)p[1]<<16 | (unsigned long)p[2]<<8 | p[3]; }
static unsigned int get16(p) unsigned char *p;
{ return (unsigned int)p[0]<<8 | p[1]; }
static void put16(p,v) unsigned char *p; unsigned int v;
{ p[0]=v>>8;p[1]=v; }
static void copy(d,s,n) unsigned char *d,*s;int n;
{ while(n--) *d++=*s++; }
static int same(a,b,n) unsigned char *a,*b;int n;
{ while(n--) if(*a++!=*b++)return 0; return 1; }
static unsigned long sum(p,n) unsigned char *p;int n;
{ unsigned long v=0;while(n>1){v+=get16(p);p+=2;n-=2;}if(n)v+=(unsigned int)*p<<8;return v; }
static unsigned int fold(v) unsigned long v;
{ while(v>>16)v=(v&65535)+(v>>16);return v; }
static int address(v) unsigned long v;
{ return v && v!=0xffffffffUL && (v>>24)!=127 && (v>>28)<14; }
int otb_identity_valid(i) struct otb_identity *i;
{
 unsigned long a=get32(i->address),m=get32(i->mask),r=get32(i->router),v;int n;
 if(get32(i->version)!=1 || !address(a) || !m)return 0;
 v=(~m)&0xffffffffUL;if(v&(v+1))return 0;
 if(r && (!address(r) || (r&m)!=(a&m)))return 0;
 for(n=0;n<3;n++){v=get32(i->dns+4*n);if(v && !address(v) && (v>>24)!=127)return 0;}
 for(n=0;n<64 && i->domain[n];n++)if(i->domain[n]<33 || i->domain[n]>126)return 0;
 return n<64;
}
static void ethernet(out,mac,type) unsigned char *out,*mac;int type;
{ copy(out,mac,6);copy(out+6,mac,6);out[11]^=1;put16(out+12,type); }
int otb_anchor_reply(i,mac,p,n,out,cap)
 struct otb_identity *i;unsigned char *mac,*p,*out;int n,cap;
{
 unsigned char *ip,*udp,*boot,*q,*end,*b,*opt;int ihl,total,ulen,msg=0,hasreq=0,hasserver=0,ended=0;
 unsigned char req[4],server[4];int reply,k,olen,blen,len;unsigned long c;
 if(!otb_identity_valid(i)||n<14||n>1514||cap<14||!same(p+6,mac,6))return -1;
 if(get16(p+12)==0x806){
  if(n<42||get16(p+14)!=1||get16(p+16)!=0x800||p[18]!=6||p[19]!=4)return -1;
  if(get16(p+20)!=1 || !get32(i->router) || !same(p+38,i->router,4) || same(p+38,i->address,4))return 0;
  if(!same(p+22,mac,6)||cap<42)return -1;
  if(get32(p+28) && !same(p+28,i->address,4))return 0;
  ethernet(out,mac,0x806);copy(out+14,p+14,28);put16(out+20,2);
  copy(out+22,out+6,6);copy(out+28,i->router,4);copy(out+32,mac,6);copy(out+38,p+28,4);return 42;
 }
 if(get16(p+12)!=0x800)return 0;
 if(n<34)return -1;ip=p+14;ihl=(ip[0]&15)*4;
 if((ip[0]>>4)!=4||ihl<20||ihl>60||n<14+ihl)return -1;
 total=get16(ip+2);if(total<ihl||total>n-14)return -1;
 if(ip[9]!=17)return 0;
 if(get16(ip+6)&0xbfff)return -1;
 if(fold(sum(ip,ihl))!=65535||total<ihl+8)return -1;
 udp=ip+ihl;ulen=get16(udp+4);if(ulen<8||ulen!=total-ihl)return -1;
 if(get16(udp)!=68||get16(udp+2)!=67)return 0;
 if(get16(udp+6) && fold(sum(ip+12,8)+17+ulen+sum(udp,ulen))!=65535)return -1;
 if(ulen<248)return -1;boot=udp+8;
 if(boot[0]!=1||boot[1]!=1||boot[2]!=6||!same(boot+28,mac,6)||get32(boot+236)!=0x63825363UL)return -1;
 q=boot+240;end=udp+ulen;
 while(q<end){k=*q++;if(k==255){ended=1;break;}if(!k)continue;if(q>=end)return -1;olen=*q++;if(olen>end-q)return -1;
  if(k==53){if(olen!=1||msg)return -1;msg=*q;}
  if(k==50){if(olen!=4||hasreq)return -1;copy(req,q,4);hasreq=1;}
  if(k==54){if(olen!=4||hasserver)return -1;copy(server,q,4);hasserver=1;}
  q+=olen;
 }
 if(!ended)return -1;
 if(msg!=1&&msg!=3&&msg!=8)return 0;
 if(msg==3 && !hasreq && !get32(boot+12))return 0;
 if(hasserver&&!same(server,i->address,4))return 0;
 reply=msg==1?2:5;
 if(msg==3 && ((hasreq&&!same(req,i->address,4)) || (get32(boot+12)&&!same(boot+12,i->address,4))))reply=6;
 if(msg==8 && !same(boot+12,i->address,4))return 0;
 /* Fixed output bounds cover every supported option. */
 if(cap<450)return -1;
 ethernet(out,mac,0x800);for(k=0;k<6;k++)out[k]=255;
 ip=out+14;for(k=0;k<20;k++)ip[k]=0;ip[0]=0x45;ip[8]=64;ip[9]=17;
 copy(ip+12,i->address,4);for(k=16;k<20;k++)ip[k]=255;
 udp=ip+20;put16(udp,67);put16(udp+2,68);put16(udp+6,0);
 b=udp+8;for(k=0;k<240;k++)b[k]=0;b[0]=2;b[1]=1;b[2]=6;
 copy(b+4,boot+4,4);copy(b+10,boot+10,2);copy(b+28,mac,6);
 if(reply!=6 && msg!=8)copy(b+16,i->address,4);
 copy(b+20,i->address,4);b[236]=99;b[237]=130;b[238]=83;b[239]=99;
 opt=b+240;*opt++=53;*opt++=1;*opt++=reply;
 *opt++=54;*opt++=4;copy(opt,i->address,4);opt+=4;
 if(reply!=6){
  *opt++=1;*opt++=4;copy(opt,i->mask,4);opt+=4;
  if(get32(i->router)){*opt++=3;*opt++=4;copy(opt,i->router,4);opt+=4;}
  olen=get32(i->dns+8)?12:get32(i->dns+4)?8:get32(i->dns)?4:0;
  if(olen){*opt++=6;*opt++=olen;copy(opt,i->dns,olen);opt+=olen;}
  for(olen=0;i->domain[olen];olen++);if(olen){*opt++=15;*opt++=olen;copy(opt,i->domain,olen);opt+=olen;}
  if(msg!=8){*opt++=51;*opt++=4;*opt++=0;*opt++=0;*opt++=14;*opt++=16;}
 }
 *opt++=255;blen=opt-b;ulen=blen+8;total=ulen+20;len=total+14;
 put16(udp+4,ulen);put16(ip+2,total);c=fold(sum(ip,20));put16(ip+10,(unsigned int)(~c));return len;
}
