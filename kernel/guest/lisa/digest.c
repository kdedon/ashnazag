#include <string.h>
#include "digest.h"
static unsigned long constants[64] = {
0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,0x3956c25bUL,0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,
0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,
0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,0xc6e00bf3UL,0xd5a79147UL,0x06ca6351UL,0x14292967UL,
0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,0x682e6ff3UL,
0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL};
#define U(x) ((x)&0xffffffffUL)
#define R(x,n) U(((x)>>(n))|((x)<<(32-(n))))
static void
block(h,p)
unsigned long *h;
const unsigned char *p;
{
	unsigned long w[64],a,b,c,d,e,f,g,j,t,u;
	int i;
	for(i=0;i<16;i++) w[i]=((unsigned long)p[i*4]<<24)|((unsigned long)p[i*4+1]<<16)|((unsigned long)p[i*4+2]<<8)|p[i*4+3];
	for(i=16;i<64;i++) w[i]=U(w[i-16]+(R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10)));
	a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];j=h[7];
	for(i=0;i<64;i++) {
		t=U(j+(R(e,6)^R(e,11)^R(e,25))+((e&f)^((~e)&g))+constants[i]+w[i]);
		u=U((R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c)));
		j=g;g=f;f=e;e=U(d+t);d=c;c=b;b=a;a=U(t+u);
	}
	h[0]=U(h[0]+a);h[1]=U(h[1]+b);h[2]=U(h[2]+c);h[3]=U(h[3]+d);
	h[4]=U(h[4]+e);h[5]=U(h[5]+f);h[6]=U(h[6]+g);h[7]=U(h[7]+j);
}
int
lisa_digest(p,n,expected)
const unsigned char *p;
unsigned long n;
const char *expected;
{
	unsigned long h[8]={0x6a09e667UL,0xbb67ae85UL,0x3c6ef372UL,0xa54ff53aUL,0x510e527fUL,0x9b05688cUL,0x1f83d9abUL,0x5be0cd19UL};
	unsigned char tail[128];
	unsigned long full=n&~63UL,rest=n-full,bytes,i,value;
	char hex[]="0123456789abcdef",out[65];
	for(i=0;i<full;i+=64) block(h,p+i);
	memset(tail,0,sizeof tail);memcpy(tail,p+full,rest);tail[rest]=0x80;
	bytes=rest<56?64:128;
	value=n>>29;
	for(i=0;i<4;i++) tail[bytes-8+i]=value>>(24-i*8);
	value=n<<3;
	for(i=0;i<4;i++) tail[bytes-4+i]=value>>(24-i*8);
	block(h,tail);if(bytes==128) block(h,tail+64);
	for(i=0;i<64;i++) out[i]=hex[(h[i/8]>>(28-(i%8)*4))&15];
	out[64]=0;
	return !strcmp(out,expected);
}
