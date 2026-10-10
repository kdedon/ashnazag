#include <assert.h>
#include "probe.c"
int main(void)
{
    unsigned char b[82]; int i;
    for(i=0;i<82;i++)b[i]=0xa5;
    pbput4(b+19,0x12345678UL);
    assert(b[18]==0xa5 && b[23]==0xa5);
    assert(b[19]==0x12 && b[20]==0x34 && b[21]==0x56 && b[22]==0x78);
    assert(pbget4(b+19)==0x12345678UL);
    pbput4(b+19,0xffffffffUL);assert(pbget4(b+19)==0xffffffffUL);
    pbput4(b+19,0);assert(pbget4(b+19)==0);
    return 0;
}
