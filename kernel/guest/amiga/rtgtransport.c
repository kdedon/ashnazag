#include <string.h>
#include "rtgshare.h"

void mig_rtg_init(struct mig_rtg *s, unsigned int width, unsigned int height)
{
    memset(s, 0, sizeof *s);
    s->magic = MIG_RTG_MAGIC;
    s->version = MIG_RTG_VERSION;
    s->header_size = MIG_RTG_HEADER_SIZE;
    s->memory_size = MIG_RTG_SIZE;
    s->max_width = width;
    s->max_height = height;
    s->format = MIG_RTG_INDEX8;
}

int mig_rtg_snapshot(const volatile struct mig_rtg *s, struct mig_rtg *out,
    unsigned int width, unsigned int height)
{
    unsigned int seq = s->seq;
    if (seq & 1) return -1;
    MIG_RTG_BARRIER();
    memcpy(out, (const void *)s, sizeof *out);
    MIG_RTG_BARRIER();
    if (seq != s->seq || seq != out->seq) return -1;
    if (out->magic != MIG_RTG_MAGIC || out->version != MIG_RTG_VERSION ||
        out->header_size != MIG_RTG_HEADER_SIZE ||
        out->memory_size != MIG_RTG_SIZE || out->format != MIG_RTG_INDEX8 ||
        out->max_width != width || out->max_height != height)
        return -1;
    if (out->on == MIG_RTG_NATIVE) return MIG_RTG_NATIVE;
    if (out->on == MIG_RTG_BLANK) return MIG_RTG_BLANK;
    if (out->on != MIG_RTG_VISIBLE || !out->width || !out->height ||
        out->width > width || out->height > height ||
        out->stride < out->width || out->stride > MIG_RTG_SIZE ||
        out->offset > MIG_RTG_SIZE - out->width ||
        out->height - 1 > (MIG_RTG_SIZE - out->offset - out->width) / out->stride)
        return -1;
    return 1;
}
