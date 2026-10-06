#ifndef MIG_HOSTFSWIRE_H
#define MIG_HOSTFSWIRE_H
#include "hostfs.h"

#define MIG_FS_BASE 0x22000000UL
#define MIG_FS_MAP_SIZE 65536UL
#define MIG_FS_MAGIC 0x4d465331U
#define MIG_FS_VERSION 2U
#define MIG_FS_IDLE 0U
#define MIG_FS_REQUEST 1U
#define MIG_FS_REPLY 2U
#define MIG_FS_BARRIER() __asm__ __volatile__("" : : : "memory")

struct mig_fs_mailbox {
    unsigned int magic, version;
    volatile unsigned int state;
    unsigned int reserved;
    struct mig_fs_request request;
};

/* the broker's progress, for the startup screen */
#define MIG_FS_STATUS (MIG_FS_BASE + MIG_FS_MAP_SIZE - 256)
struct mig_fs_status {
    unsigned int requests, kbytes;
    char path[128];
};
typedef char mig_fs_fits[(sizeof(struct mig_fs_mailbox) <= MIG_FS_MAP_SIZE - 256) ? 1 : -1];

int mig_fs_broker(int, int, const char *, int);
int mig_fs_broker_at(int, int, const char *, int, struct mig_fs_mailbox *);
#endif
