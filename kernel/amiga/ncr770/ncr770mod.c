/*
 * The CyberStorm PPC and MK III SCSI as the ncr770 module, card 1.  It
 * serves the disks, so it never unloads.
 */
#include "sys/types.h"
#include "sys/errno.h"
#include "sys/moddefs.h"
#include "rico.h"
#include "sd.h"

extern int ncr770present(), sd_attach();
extern bool ncr770queue();
extern void ncr770intr();
extern void (*int2_tbl[])();

static void (*int2_prev)();

static void
ncr770_int2(pcb)
char *pcb;
{
	(*int2_prev)(pcb);
	ncr770intr();
}

static int
ncr770_load()
{
	int i, s;

	if (!ncr770present())
		return ENXIO;
	if ((i = sd_attach((int (*)())0, ncr770queue, 0, (char *)0xF40000,
	    "CyberStorm SCSI")) != 0)
		return i;
	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	for (i = 0; int2_tbl[i + 1]; i++)
		;
	int2_prev = int2_tbl[i];
	int2_tbl[i] = ncr770_int2;
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
	return 0;
}

static int
ncr770_unload()
{
	return EBUSY;
}

MOD_MISC_WRAPPER(ncr770, ncr770_load, ncr770_unload, "CyberStorm SCSI");
