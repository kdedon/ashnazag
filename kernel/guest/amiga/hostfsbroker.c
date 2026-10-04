#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include "hostfswire.h"

int
mig_fs_broker_at(int ready, int life, const char *root, int readonly,
    struct mig_fs_mailbox *box)
{
    struct mig_hostfs *fs;
    struct mig_fs_request request;
    struct pollfd p;
    int n, status = 0;
    char success = 1;
    fs = mig_hostfs_create();
    if (!fs) return 1;
    if (mig_hostfs_mount(fs, 0, "Amiga", root, readonly) < 0) {
        fprintf(stderr, "startmig: cannot mount host directory %s\n", root);
        mig_hostfs_destroy(fs);
        return 1;
    }
    memset(box, 0, sizeof *box);
    box->magic = MIG_FS_MAGIC;
    box->version = MIG_FS_VERSION;
    MIG_FS_BARRIER();
    if (write(ready, &success, 1) != 1) {
        mig_hostfs_destroy(fs);
        return 1;
    }
    close(ready);
    p.fd = life; p.events = POLLIN;
    for (;;) {
        n = poll(&p, 1, 10);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { status = 1; break; }
        if (p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) break;
        if (box->magic != MIG_FS_MAGIC || box->version != MIG_FS_VERSION ||
            box->state > MIG_FS_REPLY) {
            status = 1; break;
        }
        if (box->state != MIG_FS_REQUEST) continue;
        MIG_FS_BARRIER();
        memcpy(&request, &box->request, sizeof request);
        mig_hostfs_dispatch(fs, &request);
        memcpy(&box->request, &request, sizeof request);
        MIG_FS_BARRIER();
        box->state = MIG_FS_REPLY;
    }
    mig_hostfs_destroy(fs);
    return status;
}

int
mig_fs_broker(int ready, int life, const char *root, int readonly)
{
    return mig_fs_broker_at(ready, life, root, readonly,
        (struct mig_fs_mailbox *)MIG_FS_BASE);
}
