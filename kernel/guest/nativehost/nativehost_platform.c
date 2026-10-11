#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "nativehost_platform.h"

#ifdef __AMIX__
#include <sys/systeminfo.h>
#if !__STDC__
extern int sysinfo();
#endif
#endif

static void
identity_copy(char *destination, const char *source, unsigned int length)
{
    unsigned int i;

    for (i = 0; i + 1 < NH_PLATFORM_TEXT && i < length && source[i]; i++)
        destination[i] = source[i];
    destination[i] = 0;
}

#ifdef __AMIX__
static void
system_text(struct nh_platform *info, int selector, unsigned long bit,
    char *destination)
{
    int length;

    info->supported |= bit;
    length = sysinfo(selector, destination, NH_PLATFORM_TEXT);
    if (length > 1 && length <= NH_PLATFORM_TEXT &&
        destination[length - 1] == 0 && destination[0])
        info->valid |= bit;
    else
        memset(destination, 0, NH_PLATFORM_TEXT);
}
#endif

#if defined(_SC_PHYS_PAGES) || defined(_SC_AVPHYS_PAGES)
static void
memory_pages(struct nh_platform *info, int selector, unsigned long bit,
    struct nh_ram_quantity *quantity)
{
    long pages;

    info->supported |= bit;
    pages = sysconf(selector);
    if (pages >= 0 && (info->valid & NH_PI_PAGE_SIZE)) {
        quantity->pages = (unsigned long)pages;
        quantity->page_bytes = info->page_bytes;
        info->valid |= bit;
    }
}
#endif

int
nh_platform_read(struct nh_platform *info)
{
    struct utsname identity;
    long pagesize;
    int saved;

    if (!info) {
        errno = EINVAL;
        return -1;
    }
    saved = errno;
    memset(info, 0, sizeof *info);
    info->supported = NH_PI_IDENTITY;
    memset(&identity, 0, sizeof identity);
    if (uname(&identity) < 0)
        return -1;
    identity_copy(info->sysname, identity.sysname, sizeof identity.sysname);
    identity_copy(info->nodename, identity.nodename, sizeof identity.nodename);
    identity_copy(info->release, identity.release, sizeof identity.release);
    identity_copy(info->version, identity.version, sizeof identity.version);
    identity_copy(info->machine, identity.machine, sizeof identity.machine);
    info->valid |= NH_PI_IDENTITY;
#ifdef __AMIX__
    system_text(info, SI_ARCHITECTURE, NH_PI_ARCHITECTURE, info->architecture);
    system_text(info, SI_HW_PROVIDER, NH_PI_PROVIDER, info->provider);
#endif
#ifdef _SC_PAGESIZE
    info->supported |= NH_PI_PAGE_SIZE;
    pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize > 0) {
        info->page_bytes = (unsigned long)pagesize;
        info->valid |= NH_PI_PAGE_SIZE;
    }
#else
    (void)pagesize;
#endif
    /* sysconf counts OS-addressable pages, distinct from installed hardware. */
#ifdef _SC_PHYS_PAGES
    memory_pages(info, _SC_PHYS_PAGES, NH_PI_MANAGED_RAM, &info->managed);
#endif
#ifdef _SC_AVPHYS_PAGES
    memory_pages(info, _SC_AVPHYS_PAGES, NH_PI_AVAILABLE_RAM, &info->available);
#endif
    errno = saved;
    return 0;
}
