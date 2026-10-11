#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include "nativehost_platform.h"

static int scenario;

int
nh_test_uname(struct utsname *identity)
{
    if (scenario == 1) {
        errno = EIO;
        return -1;
    }
    memset(identity, 0, sizeof *identity);
    strcpy(identity->sysname, "fixture-os");
    strcpy(identity->machine, "fixture-machine");
    if (scenario == 4)
        memset(identity->machine, 'm', sizeof identity->machine);
    return 0;
}

long
nh_test_sysconf(int selector)
{
    if (scenario == 2) {
        errno = EINVAL;
        return -1;
    }
    if (selector == _SC_PAGESIZE)
        return scenario == 3 ? 0 : 4096;
#ifdef _SC_PHYS_PAGES
    if (selector == _SC_PHYS_PAGES)
        return 0x7fffffffL;
#endif
#ifdef _SC_AVPHYS_PAGES
    if (selector == _SC_AVPHYS_PAGES)
        return 0;
#endif
    errno = EINVAL;
    return -1;
}

int
main(void)
{
    struct nh_platform info;

    assert(nh_platform_read(0) == -1 && errno == EINVAL);
    errno = EDOM;
    assert(nh_platform_read(&info) == 0 && errno == EDOM);
    assert(!strcmp(info.sysname, "fixture-os"));
    assert(!strcmp(info.machine, "fixture-machine"));
    assert(info.valid & NH_PI_PAGE_SIZE);
    assert(!(info.valid & NH_PI_PHYSICAL_RAM));
    assert(!info.physical.pages && !info.physical.page_bytes);
#ifdef _SC_PHYS_PAGES
    assert(info.valid & NH_PI_MANAGED_RAM);
    assert(info.managed.pages == 0x7fffffffUL);
    assert(info.managed.page_bytes == 4096);
#endif
#ifdef _SC_AVPHYS_PAGES
    assert(info.valid & NH_PI_AVAILABLE_RAM);
    assert(!info.available.pages && info.available.page_bytes == 4096);
#endif
    scenario = 1;
    memset(&info, 0xff, sizeof info);
    assert(nh_platform_read(&info) == -1 && errno == EIO);
    assert(!info.valid && !info.sysname[0] && !info.managed.pages);
    scenario = 2;
    errno = EDOM;
    assert(nh_platform_read(&info) == 0 && errno == EDOM);
    assert(info.valid == NH_PI_IDENTITY);
    assert(info.supported & NH_PI_PAGE_SIZE);
    assert(!info.page_bytes && !info.managed.pages && !info.available.pages);
    scenario = 3;
    assert(nh_platform_read(&info) == 0);
    assert(info.valid == NH_PI_IDENTITY);
    scenario = 4;
    assert(nh_platform_read(&info) == 0);
    assert(info.machine[NH_PLATFORM_TEXT - 1] == 0);
    assert(info.valid == (info.valid & info.supported));
    return 0;
}
