#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "nativehost.h"

#ifdef __AMIX__
extern int munmap();
#endif

void
nh_display_init(struct nh_display *d)
{
    memset(d, 0, sizeof *d);
    d->framebuffer = d->keyboard = d->mouse = -1;
    d->mapping = (void *)-1;
}

int
nh_display_close(struct nh_display *d)
{
    int saved = 0;

    if (!d) {
        errno = EINVAL;
        return -1;
    }
    if (d->keyboard >= 0) {
        if (close(d->keyboard) < 0)
            saved = errno;
        d->keyboard = -1;
    }
    if (d->mouse >= 0) {
        if (close(d->mouse) < 0 && !saved)
            saved = errno;
        d->mouse = -1;
    }
    /* Keep the session fd until its mapping can be released. */
    if (d->mapping != (void *)-1) {
        if (munmap(d->mapping, d->info.fi_size) < 0) {
            if (!saved)
                saved = errno;
            errno = saved;
            return -1;
        }
        d->mapping = (void *)-1;
    }
    if (d->framebuffer >= 0) {
        if (close(d->framebuffer) < 0 && !saved)
            saved = errno;
        d->framebuffer = -1;
    }
    if (saved) {
        errno = saved;
        return -1;
    }
    return 0;
}

int
nh_display_open(struct nh_display *d, const char *device, const char *name,
    int front)
{
    struct fbacq acquire;
    struct fbstate state;
    unsigned long bits;
    int saved;

    if (!d || !device || !name || (front != 0 && front != 1)) {
        errno = EINVAL;
        return -1;
    }
    if (d->framebuffer >= 0 || d->keyboard >= 0 || d->mouse >= 0 ||
        d->mapping != (void *)-1) {
        errno = EBUSY;
        return -1;
    }
    d->framebuffer = open(device, O_RDWR);
    if (d->framebuffer < 0)
        return -1;
    if (ioctl(d->framebuffer, FBIOGINFO, &d->info) < 0)
        goto failed;
    if (d->info.fi_layout != FBL_PACKED || !d->info.fi_width ||
        !d->info.fi_height || !d->info.fi_depth || d->info.fi_depth > 32 ||
        !d->info.fi_rowbytes || d->info.fi_size > (unsigned int)-1 ||
        d->info.fi_width > (unsigned long)-1 / d->info.fi_depth ||
        d->info.fi_offset >= d->info.fi_size ||
        d->info.fi_height >
        (d->info.fi_size - d->info.fi_offset) / d->info.fi_rowbytes) {
        errno = EINVAL;
        goto failed;
    }
    bits = d->info.fi_width * d->info.fi_depth;
    if (bits / 8 + (bits % 8 != 0) > d->info.fi_rowbytes) {
        errno = EINVAL;
        goto failed;
    }
    memset(&acquire, 0, sizeof acquire);
    acquire.fa_kind = FBK_USER;
    acquire.fa_flags = front ? FBA_FRONT : 0;
    strncpy(acquire.fa_name, name, sizeof acquire.fa_name - 1);
    if (ioctl(d->framebuffer, FBIOACQUIRE, &acquire) < 0)
        goto failed;
    if (front) {
        if (ioctl(d->framebuffer, FBIOGSTATE, &state) < 0)
            goto failed;
        if (state.st_session <= 0 || state.st_front != state.st_session) {
            errno = EBUSY;
            goto failed;
        }
    }
    d->mapping = mmap(0, d->info.fi_size, PROT_READ | PROT_WRITE,
        MAP_SHARED, d->framebuffer, 0);
    if (d->mapping == (void *)-1)
        goto failed;
    d->keyboard = open("/dev/kbd", O_RDONLY | O_NONBLOCK);
    if (d->keyboard < 0 ||
        ioctl(d->keyboard, EVIOCBIND, d->framebuffer) < 0)
        goto failed;
    d->mouse = open("/dev/mouse", O_RDONLY | O_NONBLOCK);
    if (d->mouse < 0 || ioctl(d->mouse, EVIOCBIND, d->framebuffer) < 0)
        goto failed;
    return 0;
failed:
    saved = errno;
    nh_display_close(d);
    errno = saved;
    return -1;
}

int
nh_input(int fd, struct inev *event)
{
    struct pollfd p;
    int n;

    if (fd < 0 || !event) {
        errno = EINVAL;
        return -1;
    }
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    n = poll(&p, 1, 0);
    if (n < 0)
        return -1;
    if (!n)
        return 0;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        errno = EIO;
        return -1;
    }
    if (!(p.revents & POLLIN))
        return 0;
    n = read(fd, event, sizeof *event);
    if (n == sizeof *event)
        return 1;
    if (n < 0 && errno == EAGAIN)
        return 0;
    if (n >= 0)
        errno = EIO;
    return -1;
}

int
nh_clock_read(struct nh_clock *clock)
{
    struct tms accounting;
    clock_t ticks;
    long hz;

    if (!clock) {
        errno = EINVAL;
        return -1;
    }
    hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        errno = EINVAL;
        return -1;
    }
    ticks = times(&accounting);
    if (ticks == (clock_t)-1)
        return -1;
    clock->ticks = (unsigned long)ticks;
    clock->frequency = hz;
    return 0;
}

void *
nh_memory(unsigned int size, int executable)
{
    int fd, saved, protection;
    void *mapping;

    if (!size || (executable != 0 && executable != 1)) {
        errno = EINVAL;
        return (void *)-1;
    }
    fd = open("/dev/zero", O_RDWR);
    if (fd < 0)
        return (void *)-1;
    protection = PROT_READ | PROT_WRITE;
    if (executable)
        protection |= PROT_EXEC;
    mapping = mmap(0, size, protection, MAP_PRIVATE, fd, 0);
    saved = errno;
    close(fd);
    errno = saved;
    return mapping;
}

long
nh_file_transfer(int fd, off_t offset, void *buffer, unsigned int count,
    int writing)
{
    unsigned int done = 0;
    int n;

    if (fd < 0 || offset < 0 || count > INT_MAX || (count && !buffer) ||
        (writing != 0 && writing != 1)) {
        errno = EINVAL;
        return -1;
    }
    if (!count)
        return 0;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    while (done < count) {
        if (writing)
            n = write(fd, (char *)buffer + done, count - done);
        else
            n = read(fd, (char *)buffer + done, count - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return done ? (long)done : -1;
        if (!n) {
            if (writing) {
                errno = EIO;
                return done ? (long)done : -1;
            }
            break;
        }
        done += n;
    }
    return (long)done;
}
