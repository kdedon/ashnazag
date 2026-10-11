/*
 * migfloppy.c -- the floppy helper: trackdisk.device's units on a host
 * drive (the floppy service's raw tracks) or on an ADF file.  A drive
 * is claimed while the helper lives.  The written track is kept until
 * UPDATE, MOTOR off, a status poll a second later, another track or exit.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include "amigaio.h"
#include "flio.h"
#include "amfm.h"
#include "floppyshare.h"
#include "miglog.h"

#define TDERR_NOTSPECIFIED 20
#define TDERR_WRITEPROT 28
#define TDERR_DISKCHANGED 29
#define TDERR_SEEKERROR 30
#define IOERR_BADLENGTH (-4)
#define RETRIES 3

struct unit {
    int fd, nsec;
    long track;			/* the kept track, -1 none */
    int dirty;
    long idle;			/* ms of the last access */
    unsigned long changes;
    unsigned char data[22 * 512];
};
static struct unit units[MIG_FD_UNITS];
static unsigned char raw[FL_MAXRAW + 2];

static long
at(int fd, void *b, long n, long off, int wr)
{
    if (lseek(fd, off, SEEK_SET) != off)
        return -1;
    return wr ? write(fd, b, n) : read(fd, b, n);
}

static int
drivestatus(struct unit *u, volatile struct mig_fd_unit *s)
{
    struct flinfo fi;

    if (ioctl(u->fd, FLIOC_INFO, &fi) < 0)
        return TDERR_NOTSPECIFIED;
    if (fi.fi_changes != u->changes) {
        u->changes = fi.fi_changes;
        u->track = -1;		/* the kept track, written or not, was the old disk's */
        u->dirty = 0;
    }
    u->nsec = fi.fi_flags & FLI_HD ? 22 : 11;
    s->flags = (fi.fi_flags & FLI_DISK ? MIG_FD_DISK : 0) |
        (fi.fi_flags & FLI_WPROT ? MIG_FD_WPROT : 0) | (fi.fi_flags & FLI_HD ? MIG_FD_HD : 0);
    s->sectors = u->nsec;
    s->changes = u->changes;
    return 0;
}

static int
rawread(struct unit *u, long track, long len)
{
    struct flraw fr;

    fr.fr_track = track;
    fr.fr_len = len;
    fr.fr_flags = 0;
    fr.fr_buf = (char *)raw;
    return ioctl(u->fd, FLIOC_READ, &fr) < 0 ? TDERR_SEEKERROR : 0;
}

static int
rawwrite(struct unit *u, long track, const unsigned char *b, long len, int index)
{
    struct flraw fr;

    fr.fr_track = track;
    fr.fr_len = len;
    fr.fr_flags = index ? FLR_INDEX : 0;
    fr.fr_buf = (char *)b;
    if (ioctl(u->fd, FLIOC_WRITE, &fr) == 0)
        return 0;
    return errno == EROFS || errno == EBADF ? TDERR_WRITEPROT : TDERR_NOTSPECIFIED;
}

static int
flush(struct unit *u, int kind)
{
    long n;

    if (!u->dirty)
        return 0;
    u->dirty = 0;
    if (kind == MIG_FD_FILE) {
        n = (long)u->nsec * 512;
        return at(u->fd, u->data, n, u->track * n, 1) == n ? 0 : TDERR_WRITEPROT;
    }
    n = amfm_encode((int)u->track, u->nsec, u->data, raw);
    return rawwrite(u, u->track, raw, n, 0);
}

/* the track's sectors into the kept buffer */
static int
load(struct unit *u, int kind, long track)
{
    long n = (long)u->nsec * 512;
    int e = 0, i;

    if (u->track == track)
        return 0;
    if ((e = flush(u, kind)) != 0)
        return e;
    u->track = -1;
    if (kind == MIG_FD_FILE) {
        if (at(u->fd, u->data, n, track * n, 0) != n)
            return TDERR_SEEKERROR;
    } else {
        for (i = 0; i < RETRIES; i++)
            if ((e = rawread(u, track, AMFM_RLEN(u->nsec))) == 0 &&
                (e = amfm_decode(raw, AMFM_RLEN(u->nsec), (int)track, u->nsec, u->data)) == 0)
                break;
        if (e) {
            miglog(1, "floppy: track %ld: error %d", track, e);
            return e;
        }
    }
    u->track = track;
    return 0;
}

static int
serve(struct mig_fd *b, unsigned char *data)
{
    struct unit *u;
    volatile struct mig_fd_unit *s;
    unsigned int kind;
    long n;
    int e;

    b->actual = 0;
    if (b->unit >= MIG_FD_UNITS || (kind = b->u[b->unit].kind) == MIG_FD_NONE)
        return 32;
    u = &units[b->unit];
    s = &b->u[b->unit];
    if (b->op != MIG_FD_STATUS)
        u->idle = miglog_ms();
    if (b->op != MIG_FD_STATUS && b->op != MIG_FD_MOTOR && b->op != MIG_FD_CLEAR &&
        !(s->flags & MIG_FD_DISK))
        return TDERR_DISKCHANGED;
    if ((b->op == MIG_FD_READ || b->op == MIG_FD_WRITE || b->op == MIG_FD_RAWREAD ||
        b->op == MIG_FD_RAWWRITE) && b->track >= 160)
        return TDERR_SEEKERROR;
    n = (long)u->nsec * 512;
    switch (b->op) {
    case MIG_FD_STATUS:
        if (kind != MIG_FD_DRIVE)
            return 0;
        return drivestatus(u, s);
    case MIG_FD_READ:
        if ((e = load(u, kind, b->track)) != 0)
            return e;
        memcpy(data, u->data, n);
        b->actual = n;
        return 0;
    case MIG_FD_WRITE:
        if (s->flags & MIG_FD_WPROT)
            return TDERR_WRITEPROT;
        if (u->track != (long)b->track && (e = flush(u, kind)) != 0)
            return e;
        memcpy(u->data, data, n);
        u->track = b->track;
        u->dirty = 1;
        b->actual = n;
        return 0;
    case MIG_FD_UPDATE:
        return flush(u, kind);
    case MIG_FD_CLEAR:
        u->track = -1;
        u->dirty = 0;
        return 0;
    case MIG_FD_MOTOR:
        e = b->len ? 0 : flush(u, kind);
        if (kind == MIG_FD_DRIVE) {
            int on = b->len != 0;
            if (ioctl(u->fd, FLIOC_MOTOR, on) < 0 && !e)
                e = TDERR_NOTSPECIFIED;
        }
        return e;
    case MIG_FD_RAWREAD:
        if (b->len == 0 || b->len > FL_MAXRAW || (b->len & 1))
            return IOERR_BADLENGTH;
        if ((e = flush(u, kind)) != 0)
            return e;
        if (kind == MIG_FD_FILE) {
            /* the revolution the write leaves, repeating */
            long rev = AMFM_REV(u->nsec), w, i, at = 0;
            unsigned char *c;
            if ((e = load(u, kind, b->track)) != 0)
                return e;
            w = amfm_encode((int)b->track, u->nsec, u->data, raw);
            c = raw + (w - rev);
            /* synced: from sector 0's second sync word */
            if (b->flags & MIG_FD_SYNC)
                at = AMFM_GAP - (w - rev) + 6;
            for (i = 0; i < (long)b->len; i++)
                data[i] = c[(at + i) % rev];
        } else if (b->flags & MIG_FD_SYNC) {
            long want = b->len + AMFM_REV(u->nsec) + 2;
            if (want > FL_MAXRAW)
                want = FL_MAXRAW;
            if ((e = rawread(u, b->track, want)) != 0)
                return e;
            if (amfm_align(raw, want, data, b->len) != (long)b->len)
                return 21;
        } else if ((e = rawread(u, b->track, b->len)) == 0)
            memcpy(data, raw, b->len);
        else
            return e;
        b->actual = b->len;
        return 0;
    case MIG_FD_RAWWRITE:
        if (b->len == 0 || b->len > FL_MAXRAW || (b->len & 1))
            return IOERR_BADLENGTH;
        if (s->flags & MIG_FD_WPROT)
            return TDERR_WRITEPROT;
        if (u->track == (long)b->track) {
            u->track = -1;
            u->dirty = 0;
        }
        if ((e = flush(u, kind)) != 0)
            return e;
        if (kind == MIG_FD_FILE) {
            /* only AmigaDOS tracks fit in an ADF */
            if ((e = amfm_decode(data, b->len, (int)b->track, u->nsec, u->data)) != 0)
                return e;
            u->track = b->track;
            u->dirty = 1;
            e = flush(u, kind);
        } else
            e = rawwrite(u, b->track, data, b->len, (b->flags & MIG_FD_INDEX) != 0);
        if (!e)
            b->actual = b->len;
        return e;
    }
    return -3;
}

/* each unit: "drive" (the host drive of that number), an ADF file, or none */
static void
attach(struct mig_fd *b, int i, const char *spec)
{
    struct unit *u = &units[i];
    volatile struct mig_fd_unit *s = &b->u[i];
    char dev[32];
    long size;
    int ro = 0;

    u->fd = -1;
    u->track = -1;
    if (!spec)
        return;
    if (!strcmp(spec, "drive")) {
        sprintf(dev, "/dev/floppy%d", i);
        if ((u->fd = open(dev, O_RDWR)) < 0 && errno == EACCES) {
            u->fd = open(dev, O_RDONLY);
            ro = 1;
        }
        if (u->fd < 0) {
            if (errno != ENOENT && errno != ENXIO && errno != ENODEV)
                miglog(1, "DF%d: %s: %s", i, dev, errno == EBUSY ? "in use" : strerror(errno));
            return;
        }
        s->kind = MIG_FD_DRIVE;
        drivestatus(u, s);
        if (ro)
            s->flags |= MIG_FD_WPROT;
        miglog(0, "DF%d: drive %d%s", i, i, s->flags & MIG_FD_DISK ? ", disk in" : "");
        return;
    }
    if ((u->fd = open(spec, O_RDWR)) < 0) {
        u->fd = open(spec, O_RDONLY);
        ro = 1;
    }
    if (u->fd < 0) {
        miglog(1, "DF%d: %.200s: %s", i, spec, strerror(errno));
        return;
    }
    size = lseek(u->fd, 0L, SEEK_END);
    if (size != 901120L && size != 1802240L) {
        miglog(1, "DF%d: %.200s: not an ADF", i, spec);
        close(u->fd);
        u->fd = -1;
        return;
    }
    u->nsec = size == 901120L ? 11 : 22;
    s->kind = MIG_FD_FILE;
    s->flags = MIG_FD_DISK | (ro ? MIG_FD_WPROT : 0) | (u->nsec == 22 ? MIG_FD_HD : 0);
    s->sectors = u->nsec;
    s->changes = 1;
    miglog(0, "DF%d: %.200s%s", i, spec, ro ? ", read-only" : "");
}

int
mig_fd_helper(int ready, int life, int dev, char **spec)
{
    struct mig_fd *b = (struct mig_fd *)MIG_FD_BASE;
    unsigned char *data = (unsigned char *)MIG_FD_DATA;
    struct amigawait aw;
    struct pollfd p;
    char success = 1;
    int i, e, n;

    memset((char *)b, 0, sizeof *b);
    b->magic = MIG_FD_MAGIC;
    b->version = MIG_FD_VERSION;
    for (i = 0; i < MIG_FD_UNITS; i++)
        attach(b, i, spec[i]);
    MIG_FD_BARRIER();
    if (write(ready, &success, 1) != 1)
        return 1;
    close(ready);
    memset(&aw, 0, sizeof aw);
    aw.aw_pid = getppid();
    p.fd = life;
    p.events = POLLIN;
    for (;;) {
        n = poll(&p, 1, 0);
        if (n > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            break;
        if (b->state != MIG_FD_REQUEST) {
            if (ioctl(dev, AMIGAIOC_WAITN, &aw) == 0 || errno == EINTR)
                continue;
            /* ESRCH: not entered yet, or gone */
            if (poll(&p, 1, 100) > 0)
                break;
            continue;
        }
        MIG_FD_BARRIER();
        e = serve(b, data);
        /* a status poll writes back what has waited a second */
        if (b->op == MIG_FD_STATUS)
            for (i = 0; i < MIG_FD_UNITS; i++)
                if (units[i].dirty && miglog_ms() - units[i].idle > 1000)
                    flush(&units[i], b->u[i].kind);
        b->error = (unsigned int)e;
        MIG_FD_BARRIER();
        b->state = MIG_FD_REPLY;
    }
    for (i = 0; i < MIG_FD_UNITS; i++)
        if (units[i].fd >= 0) {
            flush(&units[i], b->u[i].kind);
            close(units[i].fd);
        }
    return 0;
}
