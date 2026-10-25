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

/* a device's guest RAM, mapped only by the process that made it */
struct guest_ramdev {
	struct guest_ram	*rd_ram;
	struct guest_ram	*rd_hi;		/* from rd_hioff up, 0 none */
	unsigned long		rd_hioff;
	struct proc		*rd_proc;
	pid_t			rd_pid;
};
extern int guest_ramdev_new();		/* (rd, size, flags, limit) */
extern void guest_ramdev_free();	/* (rd) */
extern int guest_ramdev_mine();		/* (rd) */
extern int guest_ramdev_mmap();		/* (rd, off) */
extern int guest_ramdev_segmap();	/* (rd, dev, off, as, addrp, len, prot, maxprot, flags) */

#endif
