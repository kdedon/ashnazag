/*
 * amigazorro.c -- Zorro boards and their interrupts, for amilib.
 */

#include "sys/types.h"
#include "amilib.h"
#include "sys/param.h"
#include "sys/immu.h"
#include "vm/page.h"

#define NAUTO	16
#define CDSIZE	68			/* struct ConfigDev */
#define L(p)	(*(unsigned long *)(p))
#define W(p)	(*(unsigned short *)(p))

extern char bootinfo[];			/* autocon[NAUTO] first */
extern void (*int2_tbl[])();
extern char *dev_kvmap(), *kmem_alloc();
extern void dev_kvunmap();
extern unsigned long vtop();
extern void __amix_aciabintr();

extern int autocon();
extern unsigned long VSIZOFMEM;

#define MAXMEM	0x8000000		/* more and segu cannot be allocated */

#define INTENA	(*(__volatile__ unsigned short *)0xDFF09A)

/* boot marks in chip RAM, two copies */
#define MK(o, v)	(((__volatile__ unsigned long *)0x1000)[(o) / 4] = \
			((__volatile__ unsigned long *)0x1FF000)[(o) / 4] = (v))
extern void bootmark();

static void (*zint2)(), (*zint6)();
static void (*int2_last)();

/*
 * Memory a board added to exec's list (a PCI bridge's "pcidma memory",
 * attribute $2000, or anything inside a board's space other than a
 * memory board's) is the board's own, never RAM.
 */
static int
boardmem(bi, m)
char *bi, *m;
{
	char *cd;
	unsigned long lo = L(m + 20), hi = L(m + 24);
	int i;

	if (W(m + 14) & 0x2000)
		return 1;
	for (i = 0, cd = bi; i < NAUTO; i++, cd += CDSIZE)
		if (L(cd + 36) && !(cd[16] & 0x20) &&		/* not ERTF_MEMLIST */
		    lo < L(cd + 32) + L(cd + 36) && hi > L(cd + 32))
			return 1;
	return 0;
}

/*
 * Before config(): board memory leaves the list, and fast regions start on
 * a page, so a region ending where the next begins (behind its 32-byte
 * MemHeader) joins it.
 */
void
amiga_bootmem(kind, bi)
int kind;
char *bi;
{
	char *m;
	int i, j;

	bootmark(2, kind);
	if (kind != 3)
		return;
	for (i = 0, m = bi + NAUTO * CDSIZE; i < 16; i++, m += 32)
		while (L(m + 24) && boardmem(bi, m)) {
			for (j = 0; j < (15 - i) * 8; j++)
				L(m + j * 4) = L(m + j * 4 + 32);
			for (; j < (16 - i) * 8; j++)
				L(m + j * 4) = 0;
		}
	for (i = 0, m = bi + NAUTO * CDSIZE; i < 16; i++, m += 32) {
		if (L(m + 24) == 0)
			continue;
		MK(20, i);
		MK(24, L(m + 20));
		if (!(W(m + 14) & 2))				/* not MEMF_CHIP */
			L(m + 20) &= ~0xFFF;
	}
}

/*
 * Runs after config(), interrupts still masked.  Memory is capped and boards AmigaOS drivers
 * left interrupting are silenced: nothing would acknowledge them before
 * their own driver starts.
 */
void
amiga_quiesce()
{
	__volatile__ unsigned char *b;
	char *a;
	long n;

	if (VSIZOFMEM > MAXMEM)
		VSIZOFMEM = MAXMEM;
	if (autocon(0x21400064, 0, &a, &n)) {			/* CyberStorm SCSI */
		((__volatile__ unsigned char *)0xF60000)[8] = 0x82;
		b = (__volatile__ unsigned char *)0xF40000;
		b[0x39 ^ 3] = 0;				/* DIEN */
		b[0x40 ^ 3] = 0;				/* SIEN0 */
		b[0x41 ^ 3] = 0;				/* SIEN1 */
	}
	if (autocon(0x12120064, 0, &a, &n)) {			/* X-Surf 100 */
		b = (__volatile__ unsigned char *)a + 0x800;	/* 8390, stride 4 */
		b[0] = 0x21;					/* stop, page 0 */
		b[0x0F * 4] = 0;				/* IMR */
		b[0x07 * 4] = 0xFF;				/* ISR */
	}
	if (autocon(0x089E0021, 0, &a, &n))			/* Mediator 4000 */
		((__volatile__ unsigned char *)a)[4] = 0;		/* INTA-D enables */
	bootmark(4, VSIZOFMEM);
}

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

/*
 * Zorro III space from 1 GB to 2 GB shows at 2 GB to 3 GB through
 * supervisor page tables of its own, filled as boards are mapped.  The
 * tables live in whole pages below 1 GB, written through the one to one
 * mapping, so a big board window takes no kernel virtual space.
 */
#define IOWIN		0x40000000
#define PTRSZ		512
#define PGTSZ		256

static unsigned long *winptr[32];	/* pointer tables, kernel addresses after */
static char *tblfree;
static int tblleft;

/* n zeroed bytes aligned to align, and their physical address */
static char *
tblalloc(n, align, pap)
int n, align;
unsigned long *pap;
{
	int skip = -(int)tblfree & (align - 1);
	page_t *pp;
	char *p;

	if (tblleft < skip + n) {
		if ((pp = page_get(4096, P_CANWAIT)) == 0)
			return 0;
		if (page_pptonum(pp) >= IOWIN >> 12) {
			if (--pp->p_keepcnt == 0)
				page_abort(pp);
			if ((p = kmem_alloc(n + align, 0)) == 0)
				return 0;
			p = (char *)((unsigned long)(p + align - 1) & ~(align - 1));
			bzero(p, n);
			*pap = vtop(p, 0);
			return p;
		}
		availrmem--;
		pages_pp_kernel++;
		/* no line from its last owner may land on it later */
		__asm__ __volatile__(".word 0xf478" : : : "memory");	/* cpusha dc */
		tblfree = (char *)(page_pptonum(pp) << 12);
		tblleft = 4096;
		skip = 0;
		bzero(tblfree, 4096);
	}
	p = tblfree + skip;
	tblfree = p + n;
	tblleft -= skip + n;
	*pap = (unsigned long)p;
	return p;
}

static int
iowin(pa, size)
unsigned long pa, size;
{
	unsigned long *root, *ptr, *pgt, va, end, t;
	int r, i;

	__asm__ __volatile__(".word 0x4e7a,0x0807; mov.l %%d0,%0"	/* movec %srp,%d0 */
	    : "=g" (root) : : "d0");
	for (va = pa + IOWIN, end = va + size; va < end; va += 4096) {
		r = va >> 25;
		if ((ptr = winptr[r - 64]) == 0) {
			ptr = (unsigned long *)tblalloc(2 * PTRSZ, PTRSZ, &t);
			if (ptr == 0)
				return -1;
			winptr[r - 64] = ptr;
			root[r] = t | 3;
		}
		i = va >> 18 & 0x7F;
		if ((pgt = (unsigned long *)ptr[128 + i]) == 0) {
			if ((pgt = (unsigned long *)tblalloc(PGTSZ, PGTSZ, &t)) == 0)
				return -1;
			ptr[128 + i] = (unsigned long)pgt;
			ptr[i] = t | 3;
		}
		/* supervisor, noncacheable serialized, used, modified, resident */
		pgt[va >> 12 & 0x3F] = (va - IOWIN) | 0xD9;
	}
	__asm__ __volatile__(".word 0xf478, 0xf518" : : : "memory");	/* cpusha dc; pflusha */
	return 0;
}

/* the low gigabyte and the top one are mapped one to one, noncacheable */
char *
plat_iomap(pa, size)
unsigned long pa, size;
{
	if (pa < 0x40000000 && size <= 0x40000000 - pa || pa >= 0xC0000000)
		return (char *)pa;
	if (pa < 0x80000000 && size <= 0x80000000 - pa)
		return iowin(pa, size) ? 0 : (char *)(pa + IOWIN);
	return dev_kvmap(pa, size, 0x40, 0);
}

void
plat_iounmap(va, size)
char *va;
unsigned long size;
{
	if ((unsigned long)va >= 0x40000000 && (unsigned long)va < 0x80000000)
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
	if (intnum == 13) {
		zint6 = fn;
		INTENA = 0xA000;
	} else {
		if (int2_last == 0) {
			for (i = 0; int2_tbl[i + 1]; i++)
				;
			int2_last = int2_tbl[i];
			int2_tbl[i] = amiga_int2;
		}
		zint2 = fn;
		INTENA = 0x8008;
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
