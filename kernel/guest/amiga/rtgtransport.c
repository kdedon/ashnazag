#include <string.h>
#include "rtgshare.h"

void mig_rtg_init(struct mig_rtg *s, unsigned int width, unsigned int height,
    unsigned int vram, unsigned int vstride, unsigned int size)
{
    memset(s, 0, sizeof *s);
    s->magic = MIG_RTG_MAGIC;
    s->version = MIG_RTG_VERSION;
    s->header_size = MIG_RTG_HEADER_SIZE;
    s->memory_size = size;
    s->vram = vram;
    s->vstride = vstride;
    s->max_width = width;
    s->max_height = height;
    s->format = MIG_RTG_INDEX8;
    s->formats = 1U << MIG_RTG_INDEX8;
}

/* bytes a pixel of a format the card may show, else 0 */
unsigned int mig_rtg_bpp(unsigned int format)
{
    switch (format) {
    case MIG_RTG_INDEX8: return 1;
    case MIG_RTG_R5G6B5: case MIG_RTG_R5G6B5PC: return 2;
    case MIG_RTG_A8R8G8B8: case MIG_RTG_B8G8R8A8: return 4;
    }
    return 0;
}

int mig_rtg_snapshot(const volatile struct mig_rtg *s, struct mig_rtg *out,
    unsigned int width, unsigned int height)
{
    unsigned int seq = s->seq, bpp, row;
    if (seq & 1) return -1;
    MIG_RTG_BARRIER();
    memcpy(out, (const void *)s, sizeof *out);
    MIG_RTG_BARRIER();
    if (seq != s->seq || seq != out->seq) return -1;
    bpp = out->format < 32 && out->formats & 1U << out->format ? mig_rtg_bpp(out->format) : 0;
    if (out->magic != MIG_RTG_MAGIC || out->version != MIG_RTG_VERSION ||
        out->header_size != MIG_RTG_HEADER_SIZE || !bpp ||
        out->max_width != width || out->max_height != height)
        return -1;
    if (out->on == MIG_RTG_NATIVE) return MIG_RTG_NATIVE;
    if (out->on == MIG_RTG_BLANK) return MIG_RTG_BLANK;
    row = out->width * bpp;
    if (out->on != MIG_RTG_VISIBLE || !out->width || !out->height ||
        out->width > width || out->height > height ||
        out->stride < row || out->stride > out->memory_size ||
        out->memory_size > MIG_RTG_VRAM_MAX + MIG_RTG_EXTRA ||
        out->offset > out->memory_size - row ||
        out->height - 1 > (out->memory_size - out->offset - row) / out->stride)
        return -1;
    return 1;
}
