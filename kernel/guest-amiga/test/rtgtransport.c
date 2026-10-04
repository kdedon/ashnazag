#include <assert.h>
#include <stdio.h>
#include "rtgshare.h"

int main(void)
{
    struct mig_rtg s, out;
    assert(sizeof(unsigned int) == 4);
    assert(sizeof(unsigned short) == 2);
    assert(sizeof s <= MIG_RTG_HEADER_SIZE);
    mig_rtg_init(&s, 1024, 768);
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == 0);
    s.on = MIG_RTG_BLANK;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == MIG_RTG_BLANK);
    s.on = 3;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.on = MIG_RTG_VISIBLE; s.width = 640; s.height = 480; s.stride = 1024;
    s.offset = 128;
    s.palette[17][2] = 0xabcd;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == 1);
    assert(out.palette[17][2] == 0xabcd);
    s.seq = 1;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.seq = 0xfffffffeU;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == 1);
    s.seq += 2;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == 1);
    s.offset = MIG_RTG_SIZE - ((s.height - 1) * s.stride + s.width);
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == 1);
    s.offset++;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.offset = 0xffffffffU;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.offset = 0; s.stride = 0;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.stride = 639;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.stride = 0xffffffffU;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.stride = 640; s.height = 0;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.height = 769;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.height = 480; s.width = 1025;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.width = 640; s.version++;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.on = MIG_RTG_BLANK;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.on = MIG_RTG_VISIBLE;
    s.version--; s.memory_size++;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.memory_size--; s.max_width++;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    s.max_width--; s.format++;
    assert(mig_rtg_snapshot(&s, &out, 1024, 768) == -1);
    puts("RTG bounds and publication checks passed");
    return 0;
}
