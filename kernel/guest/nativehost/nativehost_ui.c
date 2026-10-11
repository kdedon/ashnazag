#include <string.h>
#include <errno.h>
#include "nativehost_ui.h"

int
nh_surface_init(struct nh_surface *s, void *mapping, unsigned long size,
    const struct fbinfo *info)
{
    unsigned long bits, bytes, depth;

    if (!s) {
        errno = EINVAL;
        return -1;
    }
    memset(s, 0, sizeof *s);
    if (!info || !mapping || mapping == (void *)-1 ||
        info->fi_layout != FBL_PACKED) {
        errno = EINVAL;
        return -1;
    }
    depth = info->fi_depth;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 &&
        depth != 16 && depth != 24 && depth != 32) {
        errno = EINVAL;
        return -1;
    }
    if (!info->fi_width || !info->fi_height || !info->fi_rowbytes ||
        info->fi_width > (~0UL) / depth || info->fi_size > size ||
        info->fi_offset >= info->fi_size ||
        info->fi_height > (info->fi_size - info->fi_offset) / info->fi_rowbytes) {
        errno = EINVAL;
        return -1;
    }
    bits = info->fi_width * depth;
    bytes = bits / 8 + (bits % 8 != 0);
    if (bytes > info->fi_rowbytes) {
        errno = EINVAL;
        return -1;
    }
    s->pixels = (unsigned char *)mapping + info->fi_offset;
    s->width = info->fi_width;
    s->height = info->fi_height;
    s->stride = info->fi_rowbytes;
    s->depth = depth;
    return 0;
}

int
nh_surface_fill(struct nh_surface *s, unsigned long x, unsigned long y,
    unsigned long width, unsigned long height, unsigned long pixel)
{
    unsigned long row, column, position, shift, mask, bytes, byte;
    unsigned char *p;

    if (!s || !s->pixels || !s->depth || x > s->width || y > s->height ||
        width > s->width - x || height > s->height - y ||
        (s->depth < 32 && pixel >> s->depth) || pixel > 0xffffffffUL) {
        errno = EINVAL;
        return -1;
    }
    for (row = 0; row < height; row++) {
        p = s->pixels + (y + row) * s->stride;
        for (column = 0; column < width; column++) {
            if (s->depth < 8) {
                position = (x + column) * s->depth;
                shift = 8 - s->depth - position % 8;
                mask = ((1UL << s->depth) - 1) << shift;
                p[position / 8] = (unsigned char)((p[position / 8] & ~mask) |
                    (pixel << shift));
            } else {
                bytes = s->depth / 8;
                position = (x + column) * bytes;
                for (byte = 0; byte < bytes; byte++)
                    p[position + byte] = (unsigned char)(pixel >>
                        (8 * (bytes - byte - 1)));
            }
        }
    }
    return 0;
}
