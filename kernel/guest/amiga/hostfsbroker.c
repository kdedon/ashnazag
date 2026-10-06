#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
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
static unsigned long requests, bytes, ops[16], busyms, held, peak;

static void
summary(void)
{
    miglog(0, "fs: %lu requests, %lu KB; lock %lu unlock %lu open %lu close %lu read %lu "
        "examine %lu next %lu parent %lu dup %lu; handles %lu, peak %lu; host %lu ms, guest waited %u times",
        requests, bytes >> 10, ops[MIG_FS_LOCK], ops[MIG_FS_UNLOCK], ops[MIG_FS_OPEN], ops[MIG_FS_CLOSE],
        ops[MIG_FS_READ], ops[MIG_FS_EXAMINE], ops[MIG_FS_NEXT], ops[MIG_FS_PARENT],
        ops[MIG_FS_DUPLOCK], held, peak, busyms, progress ? progress->waits : 0);
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
    if (!r->error && r->result) {
        if (r->op == MIG_FS_LOCK || r->op == MIG_FS_OPEN || r->op == MIG_FS_DUPLOCK ||
            r->op == MIG_FS_PARENT || r->op == MIG_FS_MKDIR) {
            if (++held > peak) peak = held;
        } else if ((r->op == MIG_FS_UNLOCK || r->op == MIG_FS_CLOSE) && held)
            held--;
    }
    if (r->op == MIG_FS_OPEN) {
        if (r->error) miglog(0, "open %.500s (failed, %d)", r->path, r->error);
        else miglog(0, "open %.500s", r->path);
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

/*
 * Answers the guest's SYS: requests.  With /dev/amiga, go hangs up once the
 * guest has entered and the broker then sleeps on its doorbell; without,
 * each byte on go rings instead.
 */
int
mig_fs_broker_at(int ready, int life, int go, const char *root, int readonly,
    struct mig_fs_mailbox *box)
{
    struct mig_hostfs *fs;
    struct mig_fs_request request;
    struct pollfd p[2];
    int n, status = 0;
    pid_t guest = getppid();
    char success = 1, c;
    struct amigawait aw;
    long t0;
    memset(&aw, 0, sizeof aw);
    aw.aw_pid = guest;
    aw.aw_flags = AMIGAW_ANSWER;
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
    p[0].fd = life; p[1].fd = go;
    p[0].events = p[1].events = POLLIN;
    if (mig_fs_bell >= 0) {
        do n = poll(p, 2, -1); while (n < 0 && errno == EINTR);
        close(go);
    }
    for (;;) {
        /* sleeps until the guest rings; life ends the loop */
        n = poll(p, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { status = 1; break; }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) break;
        if (box->magic != MIG_FS_MAGIC || box->version != MIG_FS_VERSION ||
            box->state > MIG_FS_REPLY) {
            status = 1; break;
        }
        if (box->state != MIG_FS_REQUEST && mig_fs_bell < 0) {
            do n = poll(p, 2, -1); while (n < 0 && errno == EINTR);
            if (n > 0 && p[1].revents && read(go, &c, 1) != 1)
                poll(p, 1, -1);
            if (progress) progress->wakes++;
            continue;
        }
        if (box->state != MIG_FS_REQUEST) {
            if (ioctl(mig_fs_bell, AMIGAIOC_WAITN, &aw) == 0 || errno == EINTR) {
                if (progress) progress->wakes++;
                continue;
            }
            /* ESRCH: the guest has left, and life ends shortly; else no doorbell */
            if (errno == ESRCH && poll(p, 1, 2000) > 0) break;
            miglog(1, "SYS: doorbell: %s", strerror(errno));
            status = 1;
            break;
        }
        MIG_FS_BARRIER();
        memcpy(&request, &box->request, sizeof request);
        if (!requests) miglog(0, "SYS: handler started");
        t0 = miglog_ms();
        mig_hostfs_dispatch(fs, &request);
        busyms += miglog_ms() - t0;
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
mig_fs_broker(int ready, int life, int go, const char *root, int readonly)
{
    struct rlimit rl;
    /* a file per open handle */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < MIG_FS_HANDLES + 16) {
        rl.rlim_cur = rl.rlim_max < MIG_FS_HANDLES + 16 ? rl.rlim_max : MIG_FS_HANDLES + 16;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    progress = (struct mig_fs_status *)MIG_FS_STATUS;
    return mig_fs_broker_at(ready, life, go, root, readonly,
        (struct mig_fs_mailbox *)MIG_FS_BASE);
}
