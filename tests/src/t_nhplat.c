#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nativehost_platform.h"
#include "t.h"

int
main(void)
{
    struct nh_platform info;
    struct utsname identity;
    int result;
    unsigned long absent = NH_PI_PROCESSOR | NH_PI_BOARD | NH_PI_PHYSICAL_RAM;

    t_init("nativehost_platform", 10);
    t_check("null_argument", nh_platform_read(0) == -1 && errno == EINVAL,
        "%s", T_ERR);
    memset(&info, 0xff, sizeof info);
    errno = EDOM;
    result = nh_platform_read(&info);
    if (!t_check("platform_read", result == 0, "%s", T_ERR))
        return t_done();
    t_check("optional_errno", errno == EDOM, "%s", T_ERR);
    t_check("identity", uname(&identity) >= 0 &&
        !strcmp(info.sysname, identity.sysname) &&
        !strcmp(info.nodename, identity.nodename) &&
        !strcmp(info.release, identity.release) &&
        !strcmp(info.version, identity.version) &&
        !strcmp(info.machine, identity.machine), "uname matches");
    t_check("valid_supported", (info.valid & ~info.supported) == 0 &&
        (info.valid & NH_PI_IDENTITY), "supported %lx valid %lx",
        info.supported, info.valid);
    t_check("hardware_unavailable", !(info.valid & absent) &&
        !info.processor[0] && !info.board[0] && !info.physical.pages &&
        !info.physical.page_bytes, "hardware queries unavailable");
#ifdef _SC_PAGESIZE
    t_check("page_size", (info.valid & NH_PI_PAGE_SIZE) &&
        info.page_bytes == (unsigned long)sysconf(_SC_PAGESIZE),
        "page bytes %lu", info.page_bytes);
#endif
#ifdef __AMIX__
    t_check("ram_unavailable", !(info.supported &
        (NH_PI_MANAGED_RAM | NH_PI_AVAILABLE_RAM)) && !info.managed.pages &&
        !info.available.pages, "AMIX public page counters unavailable");
#endif
    printf("PLATFORM os=%s node=%s release=%s version=%s machine=%s\n",
        info.sysname, info.nodename, info.release, info.version, info.machine);
    printf("PLATFORM supported=%lx valid=%lx arch=%s sysinfo_provider=%s page=%lu\n",
        info.supported, info.valid, info.architecture, info.provider,
        info.page_bytes);
    return t_done();
}
