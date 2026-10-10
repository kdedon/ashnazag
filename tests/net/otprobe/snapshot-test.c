#include <assert.h>
#define memcpy probe_memcpy
#define auxsysr fake_auxsysr
long fake_auxsysr();
#include "probe.c"
static int mode,closed,unlinked,writes;
static unsigned char image[256];
long fake_auxsysr(op,a,b,c)
long op,a,b,c;
{
    int i;
    (void)a;
    if(op==5)return mode==1?-1:9;
    if(op==10) { unlinked++;return mode==7?-1:0; }
    if(op==6) { closed++;return 0; }
    if(op==4) {
        writes++;if(mode==2)return -1;
        for(i=0;i<c;i++)image[i]=(unsigned char)(i+17);
        return mode==3?c-1:c;
    }
    if(op==19)return mode==4?-1:0;
    if(op==3) {
        if(mode==5)return -1;
        for(i=0;i<c;i++)((unsigned char *)b)[i]=image[i];
        return mode==6?c-1:c;
    }
    assert(0);return -1;
}
int main(void)
{
    unsigned char b[256];int i;
    assert(!snapshot(0,b,4));assert(!snapshot(0x1000,b,0));
    assert(!snapshot(0x1000,b,257));assert(!snapshot(0xfffffffeUL,b,4));
    assert(!writes&&!closed&&!unlinked);
    mode=1;assert(!snapshot(0x776648,b,4));assert(!closed&&!unlinked);
    for(mode=2;mode<=6;mode++) {
        closed=unlinked=0;assert(!snapshot(0x776648,b,4));
        assert(closed==1&&unlinked==1);
    }
    mode=7;closed=unlinked=0;assert(!snapshot(0x776648,b,4));
    assert(closed==1&&unlinked==1);
    mode=0;closed=unlinked=0;assert(snapshot(0x776648,b,256));
    assert(closed==1&&unlinked==1);
    for(i=0;i<256;i++)assert(b[i]==(unsigned char)(i+17));
    assert(byteslong(b)==0x11121314UL);
    return 0;
}
