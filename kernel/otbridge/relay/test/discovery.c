#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int writes;
long otr_sys_write(int fd, const unsigned char *data, unsigned long n)
{
    const char *expected = writes ? "otrelay I dependency-free getter udp\n" :
        "otrelay I dependency-free getter tcp\n";
    if (fd != 1 || n != strlen(expected) || memcmp(data, expected, n)) abort();
    writes++;
    return n;
}
#include "../discovery.c"
int main(void)
{
    if (GetOTauxtcpInstallInfo() || GetOTauxudpInstallInfo() || writes != 2) abort();
    puts("PASS dependency-free getters trace and reject installation");
    return 0;
}
