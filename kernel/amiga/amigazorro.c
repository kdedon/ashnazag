/*
 * amigazorro.c -- Zorro boards and their interrupts, for amilib.
 */

#include "sys/types.h"
#include "amilib.h"

#define NAUTO	16
#define CDSIZE	68			/* struct ConfigDev */
#define L(p)	(*(unsigned long *)(p))
#define W(p)	(*(unsigned short *)(p))

extern char bootinfo[];			/* autocon[NAUTO] first */
extern void (*int2_tbl[])();
extern char *dev_kvmap();
extern void dev_kvunmap();
extern unsigned long vtop();
extern void __amix_aciabintr();

static void (*zint2)(), (*zint6)();
static void (*int2_last)();

int
plat_zorro(i, zb)
int i;
struct amx_zboard *zb;
{
	unsigned char *cd = (unsigned char *)bootinfo + i * CDSIZE;

	if (i < 0 || i >= NAUTO || L(cd + 32) == 0)
		return 1;
	zb->zb_pa = L(cd + 32);
	zb->zb_size = L(cd + 36);
	zb->zb_manuf = W(cd + 20);
	zb->zb_prod = cd[17];
	zb->zb_type = cd[16];
	zb->zb_flags = cd[18];
	zb->zb_serial = W(cd + 22) << 16 | W(cd + 24);
	zb->zb_diagvec = W(cd + 26);
	return 0;
}

/* the low gigabyte is mapped one to one, noncacheable */
char *
plat_iomap(pa, size)
unsigned long pa, size;
{
	if (pa < 0x40000000 && size <= 0x40000000 - pa)
		return (char *)pa;
	return dev_kvmap(pa, size, 0x40, 0);
}

void
plat_iounmap(va, size)
char *va;
unsigned long size;
{
	if ((unsigned long)va >= 0x40000000)
		dev_kvunmap(va, size);
}

unsigned long
plat_vtop(va)
unsigned long va;
{
	return vtop(va, 0);
}

unsigned long
plat_pagesize()
{
	return 4096;
}

/* runs in the last slot of the level 2 table, after the handler it displaced */
static void
amiga_int2(pcb)
char *pcb;
{
	(*int2_last)(pcb);
	if (zint2)
		(*zint2)(3);
}

void
aciabintr(pcb)
char *pcb;
{
	__amix_aciabintr(pcb);
	if (zint6)
		(*zint6)(13);
}

int
plat_zintr(intnum, fn)
int intnum;
void (*fn)();
{
	int i, s;

	if (intnum != 3 && intnum != 13)
		return -1;
	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	if (intnum == 13)
		zint6 = fn;
	else {
		if (int2_last == 0) {
			for (i = 0; int2_tbl[i + 1]; i++)
				;
			int2_last = int2_tbl[i];
			int2_tbl[i] = amiga_int2;
		}
		zint2 = fn;
	}
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
	return 0;
}

void
plat_zunintr(intnum)
int intnum;
{
	if (intnum == 13)
		zint6 = 0;
	else if (intnum == 3)
		zint2 = 0;
}
