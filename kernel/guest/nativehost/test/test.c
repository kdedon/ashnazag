#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "nativehost.h"

static int mocking, calls, fail_at, opened, mapped, unmap_fail, bad_info;
static int input_mode, transfer_calls, transfer_mode, bad_state;
static char order[32];
static unsigned char pixels[4096];
static int failures;

#define CHECK(e) do { if (!(e)) { \
    fprintf(stderr, "line %d: %s (errno %d)\n", __LINE__, #e, errno); \
    failures++; } } while (0)

int __real_open(const char *, int, ...);
int __real_close(int);
int __real_ioctl(int, unsigned long, ...);
void *__real_mmap(void *, size_t, int, int, int, off_t);
int __real_munmap(void *, size_t);
int __real_poll(struct pollfd *, nfds_t, int);
ssize_t __real_read(int, void *, size_t);
ssize_t __real_write(int, const void *, size_t);
off_t __real_lseek(int, off_t, int);

static int
step(void)
{
    if (++calls == fail_at) {
        errno = ENXIO;
        return -1;
    }
    return 0;
}

int
__wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!mocking)
        return __real_open(path, flags, mode);
    if (step() < 0)
        return -1;
    if (!strcmp(path, "/dev/fb0")) {
        CHECK(flags == O_RDWR);
        opened |= 1;
        return 10;
    }
    CHECK(flags == (O_RDONLY | O_NONBLOCK));
    if (!strcmp(path, "/dev/kbd")) {
        opened |= 2;
        return 11;
    }
    CHECK(!strcmp(path, "/dev/mouse"));
    opened |= 4;
    return 12;
}

int
__wrap_close(int fd)
{
    unsigned int bit;

    if (!mocking)
        return __real_close(fd);
    CHECK(fd >= 10 && fd <= 12);
    bit = 1U << (fd - 10);
    CHECK(opened & bit);
    opened &= ~bit;
    order[strlen(order)] = fd == 10 ? 'f' : fd == 11 ? 'k' : 'm';
    errno = EBADF;
    return 0;
}

int
__wrap_ioctl(int fd, unsigned long request, ...)
{
    struct fbinfo *info;
    struct fbacq *acquire;
    struct fbstate *state;
    va_list ap;
    void *arg;

    CHECK(mocking);
    if (step() < 0)
        return -1;
    va_start(ap, request);
    if (request == EVIOCBIND) {
        CHECK(fd == 11 || fd == 12);
        CHECK(va_arg(ap, int) == 10);
    } else {
        arg = va_arg(ap, void *);
        CHECK(fd == 10);
        if (request == FBIOGINFO) {
            info = arg;
            memset(info, 0, sizeof *info);
            info->fi_layout = FBL_PACKED;
            info->fi_width = 64;
            info->fi_height = 32;
            info->fi_depth = 8;
            info->fi_rowbytes = 64;
            info->fi_size = sizeof pixels;
            info->fi_offset = 16;
            if (bad_info == 1) info->fi_offset = info->fi_size;
            if (bad_info == 2) info->fi_height = ULONG_MAX;
            if (bad_info == 3) info->fi_width = ULONG_MAX;
            if (bad_info == 4) info->fi_rowbytes = 63;
            if (bad_info == 5) info->fi_layout = FBL_PLANES;
            if (bad_info == 6) info->fi_depth = 0;
        } else if (request == FBIOGSTATE) {
            state = arg;
            memset(state, 0, sizeof *state);
            state->st_session = bad_state == 2 ? -1 : 7;
            state->st_front = bad_state == 1 ? 6 : 7;
        } else {
            CHECK(request == FBIOACQUIRE);
            acquire = arg;
            CHECK(acquire->fa_kind == FBK_USER);
            CHECK(acquire->fa_flags == FBA_FRONT);
            CHECK(acquire->fa_name[15] == 0);
            CHECK(!strncmp(acquire->fa_name, "native test", 11));
            acquire->fa_id = 7;
        }
    }
    va_end(ap);
    return 0;
}

void *
__wrap_mmap(void *address, size_t size, int protection, int flags, int fd,
    off_t offset)
{
    if (!mocking)
        return __real_mmap(address, size, protection, flags, fd, offset);
    if (step() < 0)
        return (void *)-1;
    CHECK(!address && size == sizeof pixels && offset == 0);
    CHECK(protection == (PROT_READ | PROT_WRITE) && flags == MAP_SHARED);
    CHECK(fd == 10 && (opened & 1));
    mapped = 1;
    return pixels;
}

int
__wrap_munmap(void *address, size_t size)
{
    if (!mocking)
        return __real_munmap(address, size);
    CHECK(address == pixels && size == sizeof pixels && mapped);
    CHECK(opened & 1);
    if (unmap_fail) {
        errno = EBUSY;
        return -1;
    }
    mapped = 0;
    order[strlen(order)] = 'u';
    return 0;
}

int
__wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (!mocking)
        return __real_poll(fds, count, timeout);
    CHECK(count == 1 && timeout == 0 && fds[0].events == POLLIN);
    fds[0].revents = input_mode == 3 ? POLLNVAL : POLLIN;
    return input_mode != 0;
}

ssize_t
__wrap_read(int fd, void *buffer, size_t size)
{
    struct inev *event = buffer;

    if (!mocking)
        return __real_read(fd, buffer, size);
    if (mocking == 2) {
        CHECK(fd == 20);
        if (++transfer_calls == 1) {
            memset(buffer, 'a', 2);
            return 2;
        }
        if (transfer_calls == 2) {
            errno = EINTR;
            return -1;
        }
        memset(buffer, 'b', size);
        return size;
    }
    CHECK(size == sizeof *event);
    if (input_mode == 2) {
        errno = EAGAIN;
        return -1;
    }
    if (input_mode == 4)
        return size - 1;
    memset(event, 0, size);
    event->ie_type = IE_DROP;
    event->ie_code = 23;
    event->ie_value = 17;
    return size;
}

off_t
__wrap_lseek(int fd, off_t offset, int whence)
{
    if (!mocking)
        return __real_lseek(fd, offset, whence);
    CHECK(mocking == 2 && fd == 20 && offset == 5 && whence == SEEK_SET);
    return offset;
}

ssize_t
__wrap_write(int fd, const void *buffer, size_t size)
{
    if (!mocking)
        return __real_write(fd, buffer, size);
    CHECK(mocking == 2 && fd == 20);
    if (transfer_mode == 2)
        return 0;
    if (++transfer_calls == 1) {
        CHECK(size == 6 && !memcmp(buffer, "abcdef", 6));
        return 2;
    }
    CHECK(size == 4 && !memcmp(buffer, "cdef", 4));
    if (transfer_mode == 1) {
        errno = ENOSPC;
        return -1;
    }
    if (transfer_calls == 2) {
        errno = EINTR;
        return -1;
    }
    return size;
}

static void
transfer_test(void)
{
    char buffer[6];

    mocking = 2;
    transfer_calls = transfer_mode = 0;
    CHECK(nh_file_transfer(20, 5, "abcdef", 6, 1) == 6);
    CHECK(transfer_calls == 3);
    transfer_calls = 0;
    transfer_mode = 1;
    CHECK(nh_file_transfer(20, 5, "abcdef", 6, 1) == 2);
    CHECK(errno == ENOSPC && transfer_calls == 2);
    transfer_mode = 2;
    CHECK(nh_file_transfer(20, 5, "abcdef", 6, 1) == -1 && errno == EIO);
    transfer_calls = 0;
    CHECK(nh_file_transfer(20, 5, buffer, 6, 0) == 6);
    CHECK(transfer_calls == 3 && !memcmp(buffer, "aabbbb", 6));
}

static void
reset(void)
{
    mocking = 1;
    calls = fail_at = opened = mapped = unmap_fail = bad_info = bad_state = 0;
    memset(order, 0, sizeof order);
}

static void
display_test(void)
{
    struct nh_display display;
    int i;

    for (i = 1; i <= 9; i++) {
        reset();
        fail_at = i;
        nh_display_init(&display);
        CHECK(nh_display_open(&display, "/dev/fb0", "native test", 1) == -1);
        CHECK(errno == ENXIO && !opened && !mapped);
        CHECK(nh_display_close(&display) == 0);
    }
    for (i = 1; i <= 6; i++) {
        reset();
        bad_info = i;
        nh_display_init(&display);
        CHECK(nh_display_open(&display, "/dev/fb0", "native test", 1) == -1);
        CHECK(errno == EINVAL && calls == 2 && !opened && !mapped);
    }
    for (i = 1; i <= 2; i++) {
        reset();
        bad_state = i;
        nh_display_init(&display);
        CHECK(nh_display_open(&display, "/dev/fb0", "native test", 1) == -1);
        CHECK(errno == EBUSY && calls == 4 && !opened && !mapped);
        CHECK(!strcmp(order, "f"));
    }
    reset();
    nh_display_init(&display);
    CHECK(nh_display_open(&display, "/dev/fb0", "native test", 1) == 0);
    CHECK(calls == 9 && opened == 7 && mapped);
    CHECK(nh_display_open(&display, "/dev/fb0", "native test", 1) == -1);
    CHECK(errno == EBUSY && calls == 9 && opened == 7 && mapped);
    CHECK(nh_display_close(&display) == 0);
    CHECK(!strcmp(order, "kmuf") && !opened && !mapped);
    CHECK(nh_display_close(&display) == 0 && !strcmp(order, "kmuf"));
    reset();
    nh_display_init(&display);
    CHECK(nh_display_open(&display, "/dev/fb0", "native test", 1) == 0);
    unmap_fail = 1;
    CHECK(nh_display_close(&display) == -1 && errno == EBUSY);
    CHECK(opened == 1 && mapped && display.framebuffer == 10);
    unmap_fail = 0;
    CHECK(nh_display_close(&display) == 0);
    CHECK(!strcmp(order, "kmuf") && !opened && !mapped);
    reset();
    fail_at = 9;
    unmap_fail = 1;
    nh_display_init(&display);
    CHECK(nh_display_open(&display, "/dev/fb0", "native test", 1) == -1);
    CHECK(errno == ENXIO && opened == 1 && mapped);
    CHECK(display.framebuffer == 10 && display.mapping == pixels);
    unmap_fail = 0;
    CHECK(nh_display_close(&display) == 0 && !opened && !mapped);
    reset();
    nh_display_init(&display);
    CHECK(nh_display_open(&display, "/dev/fb0", "native test", 2) == -1);
    CHECK(errno == EINVAL && calls == 0);
}

static void
input_test(void)
{
    struct inev event;

    mocking = 1;
    for (input_mode = 0; input_mode <= 4; input_mode++) {
        int result = nh_input(11, &event);
        if (input_mode == 0 || input_mode == 2)
            CHECK(result == 0);
        else if (input_mode == 1) {
            CHECK(result == 1 && event.ie_type == IE_DROP);
            CHECK(event.ie_code == 23 && event.ie_value == 17);
        } else
            CHECK(result == -1 && errno == EIO);
    }
    CHECK(nh_input(-1, &event) == -1 && errno == EINVAL);
    CHECK(nh_input(11, 0) == -1 && errno == EINVAL);
}

static void
native_test(void)
{
    struct nh_clock a, b;
    unsigned char *memory;
    char path[] = "/tmp/nativehost-XXXXXX";
    char buffer[16];
    int fd;

    mocking = 0;
    memory = nh_memory(8192, 0);
    CHECK(memory != (void *)-1);
    if (memory != (void *)-1) {
        CHECK(memory[0] == 0 && memory[8191] == 0);
        memory[0] = 17;
        memory[8191] = 23;
        CHECK(memory[0] == 17 && memory[8191] == 23);
        CHECK(munmap(memory, 8192) == 0);
    }
    CHECK(nh_memory(0, 0) == (void *)-1 && errno == EINVAL);
    CHECK(nh_memory(8192, 2) == (void *)-1 && errno == EINVAL);
    CHECK(nh_clock_read(&a) == 0 && a.frequency > 0);
    CHECK(poll(0, 0, 40) == 0);
    CHECK(nh_clock_read(&b) == 0 && b.frequency == a.frequency);
    CHECK(b.ticks - a.ticks > 0);
    CHECK(nh_clock_read(0) == -1 && errno == EINVAL);
    fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    CHECK(unlink(path) == 0);
    CHECK(nh_file_transfer(fd, 4, "abcdef", 6, 1) == 6);
    memset(buffer, 0xff, sizeof buffer);
    CHECK(nh_file_transfer(fd, 0, buffer, sizeof buffer, 0) == 10);
    CHECK(!memcmp(buffer, "\0\0\0\0abcdef", 10));
    CHECK(nh_file_transfer(fd, 10, buffer, 1, 0) == 0);
    CHECK(nh_file_transfer(fd, -1, buffer, 1, 0) == -1 && errno == EINVAL);
    CHECK(nh_file_transfer(fd, 0, 0, 1, 0) == -1 && errno == EINVAL);
    CHECK(nh_file_transfer(fd, 0, buffer, UINT_MAX, 0) == -1 && errno == EINVAL);
    CHECK(nh_file_transfer(fd, 0, 0, 0, 1) == 0);
    CHECK(close(fd) == 0);
    CHECK(nh_file_transfer(fd, 0, buffer, 1, 0) == -1 && errno == EBADF);
}

int
main(void)
{
    display_test();
    input_test();
    transfer_test();
    native_test();
    if (failures)
        return 1;
    puts("PASS nativehost ownership, bounds, input, memory, file, timer");
    return 0;
}
