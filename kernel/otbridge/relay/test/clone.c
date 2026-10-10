#define main check_mps
#include "mps.c"
#undef main
#include "../mps.c"

int main(void)
{
    struct endpoint *entries;
    unsigned long i;
    check_mps();
    CHECK(!clones);
    entries=calloc(65526,sizeof(*entries));CHECK(entries);
    for(i=0;i<65526;i++) {
        entries[i].device=0xabcd000aUL+i;
        entries[i].next=i+1<65526 ? entries+i+1 : 0;
    }
    clones=entries;
    CHECK(clone_device(0xabcd0000UL)==0);
    CHECK(clone_device(0x12340000UL)==0x1234000aUL);
    CHECK(clone_device(0xffff0000UL)==0xffff000aUL);
    clone_remove(entries+127);
    CHECK(clone_device(0xabcd0000UL)==0xabcd0089UL);
    clone_insert(entries+127);
    CHECK(clone_device(0xabcd0000UL)==0);
    clone_remove(entries);
    CHECK(clone_device(0xabcd0000UL)==0xabcd000aUL);
    clone_insert(entries);
    clone_remove(entries+65525);
    CHECK(clone_device(0xabcd0000UL)==0xabcdffffUL);
    clone_insert(entries+65525);
    for(i=0;i<65526;i++)entries[i].device=0xffff000aUL+i;
    CHECK(clone_device(0xffff0000UL)==0);
    clones=0;free(entries);
    printf("PASS %d clone admission, exhaustion and reuse checks\n",checks);
    return 0;
}
