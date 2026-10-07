/*
 * amxplat.c -- amilib's platform part for the AMIX kernel on an Amiga.
 *
 * The Amiga-specific services come from the host kernel's platform
 * layer through these weak hooks (absent: the module still loads into
 * a kernel that lacks them, finds no boards and fails with ENXIO):
 *
 *   int  plat_zorro(i, struct amx_zboard *)   board i of the autoconfig
 *                                             list; nonzero past the end
 *   char *plat_iomap(pa, size)                supervisor, cache-inhibited
 *                                             (serialized) kernel mapping
 *   void plat_iounmap(va, size)
 *   unsigned long plat_vtop(va)               physical address of a
 *                                             kernel address
 *   int  plat_zintr(intnum, fn)               call fn(intnum) from the
 *                                             level 2 (INTB_PORTS, 3) or
 *                                             level 6 (INTB_EXTER, 13)
 *                                             handler; 0 = done
 *   void plat_zunintr(intnum)
 *
 * From the kernel: kmem_alloc/kmem_free, printf, dlm_cacheflush (DLM),
 * sleep/wakeup; delayus and hrestime when the kernel has them.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/systm.h"
#include "sys/kmem.h"
#include "amilib.h"

asm(".weak plat_zorro");
asm(".weak plat_iomap");
asm(".weak plat_iounmap");
asm(".weak plat_vtop");
asm(".weak plat_zintr");
asm(".weak plat_zunintr");
asm(".weak delayus");
asm(".weak hrestime");
asm(".weak cputype");

extern int plat_zorro(), plat_zintr();
extern char *plat_iomap();
extern void plat_iounmap(), plat_zunintr();
extern unsigned long plat_vtop();
extern void delayus();
extern long hrestime[2];		/* timestruc_t: seconds, nanoseconds */
extern long cputype;			/* 30, 40, 60: the 040/060 port */
extern void dlm_cacheflush();

/* through volatile pointers, so no compiler takes a weak symbol for set */
static int (*volatile p_zorro)() = plat_zorro;
static int (*volatile p_zintr)() = plat_zintr;
static char *(*volatile p_iomap)() = plat_iomap;
static void (*volatile p_iounmap)() = plat_iounmap;
static void (*volatile p_zunintr)() = plat_zunintr;
static unsigned long (*volatile p_vtop)() = plat_vtop;
static void (*volatile p_delayus)() = delayus;
static long *volatile p_hrestime = hrestime;
static long *volatile p_cputype = &cputype;

extern long opci_loops_us;		/* Space.c: busy loops per microsecond */

static int opci_locked, opci_wanted;
static char opci_noboards;

char *
amx_alloc(size, cansleep)
	unsigned long size;
	int cansleep;
{
	return (char *)kmem_alloc((size_t)size, cansleep ? KM_SLEEP : KM_NOSLEEP);
}

void
amx_free(p, size)
	char *p;
	unsigned long size;
{
	kmem_free((_VOID *)p, (size_t)size);
}

int
amx_spl7()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s & 0xffff;
}

void
amx_splx(s)
	int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

int
amx_ipl()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s));
	return (s >> 8) & 7;
}

void
amx_delayus(n)
	long n;
{
	volatile long i;

	if (p_delayus) {
		(*p_delayus)((int)n);
		return;
	}
	while (n-- > 0)
		for (i = opci_loops_us; i > 0; i--)
			;
}

void
amx_time(tv)
	unsigned long *tv;
{
	if (p_hrestime) {
		tv[0] = p_hrestime[0];
		tv[1] = p_hrestime[1] / 1000;
	} else
		tv[0] = tv[1] = 0;
}

void
amx_cacheflush()
{
	dlm_cacheflush();
}

int
amx_attnflags()
{
	long c = p_cputype ? *p_cputype : 30;

	if (c >= 60)
		return AFF_68010 | AFF_68020 | AFF_68030 | AFF_68040 | AFF_68060 |
		    AFF_FPU40;
	if (c >= 40)
		return AFF_68010 | AFF_68020 | AFF_68030 | AFF_68040 | AFF_FPU40;
	return AFF_68010 | AFF_68020 | AFF_68030 | AFF_68881 | AFF_68882;
}

int
amx_zorro(i, zb)
	int i;
	struct amx_zboard *zb;
{
	if (p_zorro == 0 || p_iomap == 0) {
		if (!opci_noboards)
			printf("opci: this kernel lists no Zorro boards (plat_zorro)\n");
		opci_noboards = 1;
		return -1;
	}
	return (*p_zorro)(i, zb);
}

char *
amx_iomap(pa, size)
	unsigned long pa, size;
{
	return (*p_iomap)(pa, size);
}

void
amx_iounmap(va, size)
	char *va;
	unsigned long size;
{
	if (p_iounmap)
		(*p_iounmap)(va, size);
}

unsigned long
amx_vtop(va)
	char *va;
{
	return p_vtop ? (*p_vtop)(va) : (unsigned long)va;
}

static void
amx_zint(n)
	int n;
{
	(void)am_intrun(n);
}

int
amx_intattach(n)
	int n;
{
	if (p_zintr == 0)
		return -1;
	return (*p_zintr)(n, amx_zint);
}

void
amx_intdetach(n)
	int n;
{
	if (p_zunintr)
		(*p_zunintr)(n);
}

/* one caller inside the library at a time */
int
amx_lock()
{
	int s = amx_spl7();

	while (opci_locked) {
		opci_wanted = 1;
		(void)sleep((caddr_t)&opci_locked, PZERO);
	}
	opci_locked = 1;
	amx_splx(s);
	return 0;
}

int
amx_trylock()
{
	int s = amx_spl7(), e = -1;

	if (!opci_locked) {
		opci_locked = 1;
		e = 0;
	}
	amx_splx(s);
	return e;
}

void
amx_unlock()
{
	int s = amx_spl7();

	opci_locked = 0;
	if (opci_wanted) {
		opci_wanted = 0;
		wakeup((caddr_t)&opci_locked);
	}
	amx_splx(s);
}

void
amx_log(f, a, b, c, d)
	char *f;
	long a, b, c, d;
{
	printf(f, a, b, c, d);
}
