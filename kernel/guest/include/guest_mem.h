#ifndef GUEST_MEM_H
#define GUEST_MEM_H

#define GMR_CONTIG 1
#define GMR_WT 2		/* write-through: for a guest that never pushes the caches */
struct guest_ram;
extern int guest_ram_new();
extern int guest_ram_frame();
extern int guest_ram_map();
extern unsigned long guest_ram_size();
extern int guest_ram_phys();
extern void guest_ram_free();
extern void guest_ram_invalidator();

#endif
