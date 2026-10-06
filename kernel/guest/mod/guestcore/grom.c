/*
 * grom.c -- a host machine's own ROM, mapped in place into a guest.
 *
 * Each host platform finds its ROM through a hook with one contract:
 * romva(n) is the kernel address of the first n bytes of the machine's
 * ROM, 0 if it has none or n is beyond it (mac_romva, ata_romva,
 * amiga_romva).  An environment module checks the ROM through its hook,
 * then guest_rommap() maps the ROM's own pages at the same address in
 * the caller, read and execute only.  Without a usable host ROM the
 * environment loads its ROM file instead.
 *
 * K&R C.
 */

#include "kinc.h"
#include "sys/mman.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "vm/seg_dev.h"

extern int segdev_create(), as_map(), as_unmap(), valid_usr_range();
/* only the 68040 HAT, whose page frames are 4 KB, has hat_cm_ram */
__asm__(".weak hat_cm_ram");
extern long hat_cm_ram;
static long *volatile gr_hat4k = &hat_cm_ram;
/* the 68040 HAT caches a device frame only when it is registered */
__asm__(".weak hat_cm_rom_add");
extern int hat_cm_rom_add();
static int (*volatile gr_romadd)() = hat_cm_rom_add;

#define	GR_MAX	4

/* ROM ranges handed out so far: the map function serves nothing else */
static struct {
	unsigned long	pa, size;
} gr_rom[GR_MAX];

/* segdev's map function: the offset is the physical address */
static int
gr_mmap(dev, off, prot)
	dev_t dev;
	off_t off;
	int prot;
{
	int i;

	if (prot & PROT_WRITE)
		return -1;
	for (i = 0; i < GR_MAX; i++)
		if (gr_rom[i].size && (unsigned long)off >= gr_rom[i].pa &&
		    (unsigned long)off - gr_rom[i].pa < gr_rom[i].size)
			return (int)((unsigned long)off >> (gr_hat4k ? 12 : 11));
	return -1;
}

/*
 * The host ROM at physical pa, size bytes, at the same user address in
 * the caller.  dev: the environment's device, which the mapping holds.
 */
int
guest_rommap(dev, pa, size)
	dev_t dev;
	unsigned long pa, size;
{
	struct segdev_crargs a;
	struct as *as = u.u_procp->p_as;
	int i;

	if (((pa | size) & PAGEOFFSET) || size == 0 || !valid_usr_range((caddr_t)pa, size))
		return EINVAL;
	for (i = 0; i < GR_MAX; i++)
		if (gr_rom[i].pa == pa && gr_rom[i].size == size)
			break;
	if (i == GR_MAX)
		for (i = 0; i < GR_MAX && gr_rom[i].size; i++)
			;
	if (i < GR_MAX) {
		gr_rom[i].pa = pa;
		gr_rom[i].size = size;
	}
	if (i == GR_MAX)
		return ENOMEM;
	if (gr_romadd)
		(void)(*gr_romadd)(pa >> 12, (pa + size) >> 12);
	(void)as_unmap(as, (caddr_t)pa, size);
	a.mapfunc = gr_mmap;
	a.offset = pa;
	a.dev = dev;
	a.prot = a.maxprot = PROT_READ | PROT_EXEC | PROT_USER;
	return as_map(as, (caddr_t)pa, size, segdev_create, (caddr_t)&a);
}

/* nonzero once a ROM was mapped: its mappings may outlive the device */
int
guest_romheld()
{
	return gr_rom[0].size != 0;
}
