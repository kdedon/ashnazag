#ifndef NATIVEHOST_PLATFORM_H
#define NATIVEHOST_PLATFORM_H

#define NH_PLATFORM_TEXT 257
#define NH_PI_IDENTITY       0x0001UL
#define NH_PI_ARCHITECTURE   0x0002UL
#define NH_PI_PROVIDER       0x0004UL
#define NH_PI_PROCESSOR      0x0008UL
#define NH_PI_BOARD          0x0010UL
#define NH_PI_PAGE_SIZE      0x0020UL
#define NH_PI_PHYSICAL_RAM   0x0040UL
#define NH_PI_MANAGED_RAM    0x0080UL
#define NH_PI_AVAILABLE_RAM  0x0100UL

/* Keep page counts separate to avoid byte-count overflow on 32-bit hosts. */
struct nh_ram_quantity {
    unsigned long pages;
    unsigned long page_bytes;
};

struct nh_platform {
    unsigned long supported, valid;
    char sysname[NH_PLATFORM_TEXT], nodename[NH_PLATFORM_TEXT];
    char release[NH_PLATFORM_TEXT], version[NH_PLATFORM_TEXT];
    char machine[NH_PLATFORM_TEXT], architecture[NH_PLATFORM_TEXT];
    char provider[NH_PLATFORM_TEXT], processor[NH_PLATFORM_TEXT];
    char board[NH_PLATFORM_TEXT];
    unsigned long page_bytes;
    /* Installed hardware, OS-addressable pages, and currently available pages. */
    struct nh_ram_quantity physical, managed, available;
};

/* supported: compiled query; valid: successful sample. Absent fields are zero.
 * Returns 0 after uname succeeds, -1 with errno on identity or argument failure.
 * machine is the OS-reported identity; processor and board need separate queries.
 * architecture and provider contain OS sysinfo labels.
 */
int nh_platform_read(struct nh_platform *);

#endif
