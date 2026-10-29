/*
 * The A4000's IDE port as the amigaide module: card 0, its master
 * answering as unit 6 (c6d0).  It serves the disks, so it never
 * unloads.
 */
#include "sys/types.h"
#include "sys/errno.h"
#include "sys/moddefs.h"
#include "rico.h"
#include "sd.h"

/* the card the kernel holds for the port, whatever its task file address */
#define IDECARD	((char *)0xDA0000)

extern int idepresent(), idequeue(), sd_attach();
extern void ideintr(), dlm_cachepush();
extern void (*int2_tbl[])();
/* RAM above the kernel's region: its bus and kernel addresses differ */
__asm__(".weak hr_tobus");
__asm__(".weak hr_tokern");
extern unsigned long hr_tobus(), hr_tokern();
static unsigned long (*__volatile__ ide_tobus)() = hr_tobus;
static unsigned long (*__volatile__ ide_tokern)() = hr_tokern;

/* ide.c runs interrupt driven only when it finds itself in this table */
void (*ide_int2_tbl[])() = { ideintr, 0 };

static void (*int2_prev)();

/*
 * PIO goes through the uncached one to one view, at the buffer's kernel
 * address; keep the cache out of it.
 */
static bool
ideq(c, cp)
struct sdcom *cp;
{
	if (ide_tobus) {
		cp->addr = (caddr_t)(*ide_tobus)(cp->addr);
		dlm_cachepush(cp->addr, cp->nbyte);
		cp->addr = (caddr_t)(*ide_tokern)(cp->addr);
	} else
		dlm_cachepush(cp->addr, cp->nbyte);
	return idequeue(c, cp);
}

/* takes the level 2 table's last slot, as the PCI hook does */
static void
ide_int2(pcb)
char *pcb;
{
	(*int2_prev)(pcb);
	ideintr();
}

static int
amigaide_load()
{
	char *a;
	int i, s;

	if (!idepresent(&a))
		return ENXIO;
	if ((i = sd_attach((int (*)())0, ideq, 0, IDECARD, "A4000 IDE")) != 0)
		return i;
	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	for (i = 0; int2_tbl[i + 1]; i++)
		;
	int2_prev = int2_tbl[i];
	int2_tbl[i] = ide_int2;
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
	return 0;
}

static int
amigaide_unload()
{
	return EBUSY;
}

MOD_MISC_WRAPPER(amigaide, amigaide_load, amigaide_unload, "A4000 IDE");
