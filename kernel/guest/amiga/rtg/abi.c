#include <stddef.h>
#include <stdio.h>
#include "../rtgshare.h"
#define FIELD(name) printf(".equ rtg_" #name ", %lu\n", (unsigned long)offsetof(struct mig_rtg, name))
int main(void)
{
    printf(".equ RTG_BASE, %lu\n", MIG_RTG_BASE);
    printf(".equ RTG_PIXELS, %lu\n", MIG_RTG_PIXELS);
    printf(".equ RTG_SIZE, %lu\n", MIG_RTG_SIZE);
    printf(".equ RTG_MAGIC, %u\n", MIG_RTG_MAGIC);
    FIELD(magic); FIELD(version); FIELD(header_size); FIELD(memory_size);
    FIELD(max_width); FIELD(max_height); FIELD(format); FIELD(seq);
    FIELD(on); FIELD(width); FIELD(height); FIELD(stride); FIELD(offset);
    FIELD(palette);
    return 0;
}
