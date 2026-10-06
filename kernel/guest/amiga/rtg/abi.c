#include <stddef.h>
#include <stdio.h>
#include "../rtgshare.h"
#define FIELD(name) printf(".equ rtg_" #name ", %lu\n", (unsigned long)offsetof(struct mig_rtg, name))
int main(void)
{
    printf(".equ RTG_BASE, %lu\n", MIG_RTG_BASE);
    printf(".equ RTG_MAGIC, %u\n", MIG_RTG_MAGIC);
    printf(".equ RTG_VERSION, %u\n", MIG_RTG_VERSION);
    FIELD(magic); FIELD(version); FIELD(header_size); FIELD(memory_size);
    FIELD(max_width); FIELD(max_height); FIELD(format); FIELD(seq);
    FIELD(on); FIELD(width); FIELD(height); FIELD(stride); FIELD(offset);
    FIELD(palette); FIELD(cursor); FIELD(cseq); FIELD(con); FIELD(cx); FIELD(cy);
    FIELD(cw); FIELD(ch); FIELD(crgb); FIELD(cimg); FIELD(vram); FIELD(vstride); FIELD(copy);
    FIELD(plock); FIELD(pad); FIELD(drawn); FIELD(pwant); FIELD(pshown); FIELD(porigin); FIELD(pstride);
    FIELD(px0); FIELD(py0); FIELD(px1); FIELD(py1); FIELD(psave); FIELD(pdrawn);
    FIELD(track);
    printf(".equ RTG_HEADER_SIZE, %lu\n", MIG_RTG_HEADER_SIZE);
    return 0;
}
