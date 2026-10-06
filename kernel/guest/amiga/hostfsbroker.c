#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include "hostfswire.h"
#include "miglog.h"
#include "amigaio.h"

int mig_fs_bell = -1;   /* /dev/amiga, for the guest's doorbell */

static volatile struct mig_fs_status *progress;
static unsigned long requests, bytes, ops[16];

static void
summary(void)
{
    miglog(0, "fs: %lu requests, %lu KB; lock %lu unlock %lu open %lu close %lu read %lu "
        "examine %lu next %lu parent %lu dup %lu", requests, bytes >> 10,
        ops[MIG_FS_LOCK], ops[MIG_FS_UNLOCK], ops[MIG_FS_OPEN], ops[MIG_FS_CLOSE],
        ops[MIG_FS_READ], ops[MIG_FS_EXAMINE], ops[MIG_FS_NEXT], ops[MIG_FS_PARENT],
        ops[MIG_FS_DUPLOCK]);
}

/* logs opened files and a summary every 5 s; shows progress */
static void
note(struct mig_fs_request *r)
{
    static long next = 5000;
    unsigned int i;
    requests++;
    ops[r->op & 15]++;
    if ((r->op == MIG_FS_READ || r->op == MIG_FS_WRITE) && !r->error && r->result > 0)
        bytes += r->result;
    if (r->op == MIG_FS_OPEN) {
        miglog(0, "open %.500s%s", r->path, r->error ? " (failed)" : "");
    }
    if (miglog_ms() >= next) {
        summary();
        next = miglog_ms() + 5000;
    }
    if (!progress) return;
    progress->requests = requests;
    progress->kbytes = bytes >> 10;
    if (r->op == MIG_FS_OPEN || r->op == MIG_FS_LOCK) {
        for (i = 0; i < sizeof progress->path - 1 && r->path[i]; i++)
            progress->path[i] = r->path[i];
        progress->path[i] = 0;
    }
}

int
mig_fs_broker_at(int ready, int life, const char *root, int readonly,
    struct mig_fs_mailbox *box)
{
    struct mig_hostfs *fs;
    struct mig_fs_request request;
    struct pollfd p;
    int n, status = 0, quiet = 0, bell = mig_fs_bell >= 0;
    pid_t guest = getppid();
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
        /*
         * With the doorbell, sleeps until the guest rings; without, polls
         * every tick while the guest uses SYS:, so a request waits one.
         */
        n = poll(&p, 1, bell ? 0 : quiet > 500 ? 20 : 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { status = 1; break; }
        if (p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) break;
        if (box->magic != MIG_FS_MAGIC || box->version != MIG_FS_VERSION ||
            box->state > MIG_FS_REPLY) {
            status = 1; break;
        }
        if (box->state != MIG_FS_REQUEST) {
            quiet++;
            /* before the guest starts and after it exits: ESRCH */
            if (bell && ioctl(mig_fs_bell, AMIGAIOC_WAIT, guest) < 0 && errno != EINTR) {
                if (errno != ESRCH) bell = 0;
                else poll(&p, 1, 20);
            }
            continue;
        }
        quiet = 0;
        MIG_FS_BARRIER();
        memcpy(&request, &box->request, sizeof request);
        if (!requests) miglog(0, "SYS: handler started");
        mig_hostfs_dispatch(fs, &request);
        memcpy(&box->request, &request, sizeof request);
        note(&request);
        MIG_FS_BARRIER();
        box->state = MIG_FS_REPLY;
    }
    mig_hostfs_destroy(fs);
    summary();
    return status;
}

int
mig_fs_broker(int ready, int life, const char *root, int readonly)
{
    progress = (struct mig_fs_status *)MIG_FS_STATUS;
    return mig_fs_broker_at(ready, life, root, readonly,
        (struct mig_fs_mailbox *)MIG_FS_BASE);
}
