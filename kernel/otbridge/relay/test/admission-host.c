#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../admission.h"
struct configuration { unsigned short children; char name[32]; unsigned long options, port; };
static int calls, queries, noname;
unsigned short OTCfigNumberOfChildren(void *p) { ++queries; return ((struct configuration *)p)->children; }
unsigned long OTCfigGetProviderNameD0(void *p) { return noname ? 0 : (unsigned long)((struct configuration *)p)->name; }
void OTCfigChangeProviderName(void *p,const char *s) { ++calls; strcpy(((struct configuration *)p)->name,s); }
int main(void)
{
    struct configuration c; unsigned long opts=0x87654321,port=0x12345678;
    const char *names[]={"tcp","udp","auxtcp","auxudp","TCP","UDP","tcp ","udpX","ip","","t","tc","u","ud"};
    unsigned int i; int tests=0;
    otr_admit_configuration(0);otr_admit_configuration((void *)-1L);
    assert(!calls && !queries);++tests;
    for(i=0;i<sizeof names/sizeof names[0];i++) {
        int before=calls;
        c.children=0;strcpy(c.name,names[i]);c.options=opts;c.port=port;
        otr_admit_configuration(&c);
        assert(c.options==opts && c.port==port);
        assert(!strcmp(c.name,i==0 ? "auxtcp" : i==1 ? "auxudp" : names[i]));
        assert(calls==before+(i<2));++tests;
        before=calls;otr_admit_configuration(&c);assert(calls==before);++tests;
        c.children=1;strcpy(c.name,names[i]);otr_admit_configuration(&c);
        assert(calls==before && !strcmp(c.name,names[i]));++tests;
    }
    c.children=0;noname=1;{int before=calls;otr_admit_configuration(&c);assert(calls==before);++tests;}
    printf("%d admission checks PASS\n",tests);return 0;
}
