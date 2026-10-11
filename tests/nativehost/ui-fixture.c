#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nativehost_ui.h"

static int failures, checks;

static void
check(int ok, const char *name)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static void
packing(unsigned long depth)
{
    unsigned char storage[128], expected[128], before[128];
    struct fbinfo info;
    struct nh_surface surface;
    unsigned long x, y, bit, at, source, pixel;

    memset(&info, 0, sizeof info);
    info.fi_layout = FBL_PACKED;
    info.fi_width = 7;
    info.fi_height = 3;
    info.fi_depth = depth;
    info.fi_rowbytes = (7 * depth + 7) / 8 + 3;
    info.fi_offset = 5;
    info.fi_size = info.fi_offset + 3 * info.fi_rowbytes;
    memset(storage, 0xa5, sizeof storage);
    memcpy(expected, storage, sizeof storage);
    pixel = depth == 32 ? 0x89abcdefUL : (1UL << (depth - 1)) | 1;
    check(nh_surface_init(&surface, storage, sizeof storage, &info) == 0,
        "surface init");
    check(nh_surface_fill(&surface, 1, 1, 5, 2, pixel) == 0, "rectangle fill");
    for (y = 1; y < 3; y++)
        for (x = 1; x < 6; x++)
            for (bit = 0; bit < depth; bit++) {
                at = (info.fi_offset + y * info.fi_rowbytes) * 8 + x * depth + bit;
                source = (pixel >> (depth - bit - 1)) & 1;
                expected[at / 8] = (unsigned char)((expected[at / 8] &
                    ~(1 << (7 - at % 8))) | (source << (7 - at % 8)));
            }
    check(!memcmp(storage, expected, sizeof storage), "pixel order and guard bytes");
    memcpy(before, storage, sizeof storage);
    check(nh_surface_fill(&surface, 6, 2, 2, 1, pixel) < 0 && errno == EINVAL,
        "rectangle bounds");
    check(nh_surface_fill(&surface, (~0UL), 0, 1, 1, pixel) < 0 && errno == EINVAL,
        "coordinate overflow");
    if (depth < 32)
        check(nh_surface_fill(&surface, 0, 0, 1, 1, 1UL << depth) < 0 &&
            errno == EINVAL, "pixel range");
    check(nh_surface_fill(&surface, 7, 3, 0, 0, pixel) == 0, "empty edge rectangle");
    check(!memcmp(storage, before, sizeof storage), "rejected fills unchanged");
    check(nh_surface_init(&surface, storage, info.fi_size - 1, &info) < 0 &&
        !surface.pixels, "short mapping");
    info.fi_width = (~0UL);
    check(nh_surface_init(&surface, storage, sizeof storage, &info) < 0,
        "width overflow");
    info.fi_width = 7;
    info.fi_rowbytes = 1;
    info.fi_height = 0;
    check(nh_surface_init(&surface, storage, sizeof storage, &info) < 0,
        "empty geometry");
    info.fi_height = 3;
    if (depth >= 2)
        check(nh_surface_init(&surface, storage, sizeof storage, &info) < 0,
            "short stride");
}

int
main(void)
{
    struct nh_surface surface;
    unsigned long depths[] = { 1, 2, 4, 8, 16, 24, 32 };
    unsigned int i;

    for (i = 0; i < sizeof depths / sizeof depths[0]; i++)
        packing(depths[i]);
    memset(&surface, 0, sizeof surface);
    check(nh_surface_fill(&surface, 0, 0, 1, 1, 0) < 0 && errno == EINVAL,
        "uninitialized surface");
    check(nh_surface_init(0, 0, 0, 0) < 0 && errno == EINVAL, "null surface");
    check(nh_surface_init(&surface, 0, 0, 0) < 0 && errno == EINVAL,
        "null mapping");
    printf("TESTS DONE pass=%d fail=%d skip=0\n", checks - failures, failures);
    return failures != 0;
}
