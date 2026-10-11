#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nativehost.h"

static unsigned char pixels[16];
static int scenario, opens, closes, mappings, releases, state_queries;
static int failures, checks;

int
nh_test_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return 10 + opens++;
}

int
nh_test_close(int fd)
{
    (void)fd;
    closes++;
    return 0;
}

void *
nh_test_mmap(void *address, size_t size, int protection, int flags, int fd,
    off_t offset)
{
    (void)address;
    (void)size;
    (void)protection;
    (void)flags;
    (void)fd;
    (void)offset;
    mappings++;
    return pixels;
}

int
nh_test_munmap(void *address, size_t size)
{
    (void)address;
    (void)size;
    releases++;
    return 0;
}

int
nh_test_ioctl(int fd, unsigned long request, ...)
{
    struct fbinfo *info;
    struct fbstate *state;
    va_list args;

    (void)fd;
    va_start(args, request);
    if (request == FBIOGINFO) {
        info = va_arg(args, struct fbinfo *);
        memset(info, 0, sizeof *info);
        info->fi_layout = FBL_PACKED;
        info->fi_width = 4;
        info->fi_height = 4;
        info->fi_depth = 8;
        info->fi_rowbytes = 4;
        info->fi_size = sizeof pixels;
    } else if (request == FBIOGSTATE) {
        state_queries++;
        state = va_arg(args, struct fbstate *);
        if (scenario == 2) {
            va_end(args);
            errno = EIO;
            return -1;
        }
        state->st_session = scenario == 3 ? -1 : 7;
        state->st_front = scenario == 1 ? 6 : 7;
    }
    va_end(args);
    return 0;
}

static void
check(int ok, const char *name)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

int
main(void)
{
    struct nh_display display;
    int result;

    for (scenario = 0; scenario < 4; scenario++) {
        opens = closes = mappings = releases = state_queries = 0;
        nh_display_init(&display);
        result = nh_display_open(&display, "/dev/fb0", "surface", 1);
        if (!scenario) {
            check(result == 0 && opens == 3 && mappings == 1 && state_queries == 1,
                "verified foreground opens input and mapping");
            check(nh_display_close(&display) == 0 && closes == 3 && releases == 1,
                "foreground release");
        } else {
            check(result < 0 && errno == (scenario == 2 ? EIO : EBUSY),
                "foreground failure reported");
            check(opens == 1 && closes == 1 && !mappings && !releases &&
                display.framebuffer == -1 && display.keyboard == -1 &&
                display.mouse == -1 && display.mapping == (void *)-1,
                "foreground failure cleans session before mapping");
        }
    }
    opens = closes = mappings = releases = state_queries = 0;
    nh_display_init(&display);
    check(nh_display_open(&display, "/dev/fb0", "surface", 0) == 0 &&
        !state_queries, "background acquisition");
    check(nh_display_close(&display) == 0 && closes == 3 && releases == 1,
        "background release");
    printf("TESTS DONE pass=%d fail=%d skip=0\n", checks - failures, failures);
    return failures != 0;
}
