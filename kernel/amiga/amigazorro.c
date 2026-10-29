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
extern unsigned long VSIZOFMEM, MAINSTORE, dlm_bootpack();
extern char end[];
extern void amiga_boottab(), dlm_bootrebase();
extern unsigned long hr_n, hr_plo, hr_phi, hr_blo, hr_bhi, hr_seg[];
extern unsigned long hr_tobus(), hr_tokern();
extern unsigned long kroot040, hat_cm_ram, kh_alias;

#define MAXMEM	0x8000000		/* the page array's limit without highram() */

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
 * Banks the card sizes wrongly show their lower half again above it.  At
 * each power-of-two offset in the region, a write at the offset that
 * shows at the start means everything from there on is an alias: the
 * region ends there.  Both words are put back.
 */
static void
trimalias(m)
char *m;
{
	__volatile__ unsigned long *a0, *a1;
	unsigned long s0, s1, lo = L(m + 20), hi = L(m + 24), k;
	int alias;

	a0 = (__volatile__ unsigned long *)((lo + 0xFFF) & ~0xFFFUL);
	for (k = 0x10000; (unsigned long)a0 + k + 4 <= hi; k <<= 1) {
		a1 = (__volatile__ unsigned long *)((char *)a0 + k);
		s0 = *a0;
		s1 = *a1;
		*a1 = 0xA5C3965A;
		__asm__ __volatile__(".word 0xf4f8");		/* cpusha %bc */
		*a0 = 0x5A3C69A5;
		__asm__ __volatile__(".word 0xf4f8");
		alias = *a1 == 0x5A3C69A5;
		*a1 = s1;
		*a0 = s0;
		__asm__ __volatile__(".word 0xf4f8");
		if (alias) {
			L(m + 24) = hi = lo + k;
			break;
		}
	}
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
	amiga_boottab(bi);
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
		if (!(W(m + 14) & 2))				/* not MEMF_CHIP */
			trimalias(m);
		MK(20, i);
		MK(24, L(m + 20));
		if (!(W(m + 14) & 2))
			L(m + 20) &= ~0xFFF;
	}
}

/*
 * Fast RAM outside the kernel's region (Zorro III RAM) gets
 * pseudo-physical addresses from the region's end E up: supervisor page
 * tables map them, noncacheable as the rest of the kernel's one to one
 * view, and the transparent window shrinks to [0, E).  A cached view of
 * the kernel's region above them holds the page array.  The tables sit
 * at the top of the last extent, below the boot modules.  Set up with
 * the MMU off; everything here is in .data.
 */
#define HR_NSEG		4
#define HR_PTR		0x2000000UL	/* a pointer table's span */
#define HR_LEAF		0x40000UL	/* a page table's span */
#define HR_PG		0x1000UL
#define HR_TOP		0x40000000UL	/* the low gigabyte */
#define HR_LAST		0x800000UL	/* the least last extent */

static unsigned long hr_e = 0;		/* E; 0: no extents */
static unsigned long hr_ptbus = 0;	/* first pointer table, bus address */
static unsigned long hr_alva = 0;	/* the cached view of the kernel's region */

/* [lo, hi) meets [a, b) */
#define HR_MEETS(lo, hi, a, b)	((lo) < (b) && (hi) > (a))

/* fill the tables at t (bus) for [va, va + len) -> pa or hr_tobus(va); returns past them, 512-aligned */
static unsigned long
hr_tables(t, va, len, pa, bits)
unsigned long t, va, len, pa, bits;
{
	unsigned long *ptr, *leaf, v, p0 = va & ~(HR_PTR - 1), l0 = va & ~(HR_LEAF - 1);
	unsigned long np = (va + len - 1 - p0) / HR_PTR + 1, nl = (va + len - 1 - l0) / HR_LEAF + 1;

	ptr = (unsigned long *)t;
	leaf = (unsigned long *)(t + np * 512);
	bzero((char *)ptr, np * 512 + nl * 256);
	for (v = l0; v < va + len; v += HR_LEAF, leaf += 64)
		ptr[(v - p0) / HR_LEAF] = (unsigned long)leaf | 2;
	leaf = (unsigned long *)(t + np * 512);
	for (v = va; v < va + len; v += HR_PG)
		leaf[(v - l0) / HR_PG] = (pa ? pa + (v - va) : hr_tobus(v)) | bits;
	return (t + np * 512 + nl * 256 + 511) & ~511UL;
}

static void
highram()
{
	unsigned long e = MAINSTORE + VSIZOFMEM, lo, hi, tot = 0, a, k, t, sz;
	unsigned long sb[HR_NSEG], ss[HR_NSEG];
	char *m;
	int i, j, ns = 0;

	if (e < 0x1000000 || (e & (e - 1)))
		return;
	for (i = 0, m = bootinfo + NAUTO * CDSIZE; i < 16; i++, m += 32) {
		lo = L(m + 20);
		hi = L(m + 24) & ~(HR_PG - 1);
		if (hi <= lo || (W(m + 14) & 2) || lo < e)
			continue;
		if (ns == HR_NSEG)
			break;
		for (j = ns++; j > 0 && sb[j - 1] > lo; j--) {
			sb[j] = sb[j - 1];
			ss[j] = ss[j - 1];
		}
		sb[j] = lo;
		ss[j] = hi - lo;
		tot += hi - lo;
	}
	/* the last extent holds the boot modules and the tables */
	if (ns == 0 || ss[ns - 1] < HR_LAST)
		return;
	a = (e + tot + HR_PTR - 1) & ~(HR_PTR - 1);
	if (e + tot < e || a + VSIZOFMEM > HR_TOP || a + VSIZOFMEM < a)
		return;
	/* nothing else decodes the pseudo range or the cached view */
	for (i = 0, m = bootinfo + NAUTO * CDSIZE; i < 16; i++, m += 32)
		if (L(m + 24) && HR_MEETS(L(m + 20), L(m + 24), e, a + VSIZOFMEM))
			return;
	for (i = 0, m = bootinfo; i < NAUTO; i++, m += CDSIZE)
		if (L(m + 36) && HR_MEETS(L(m + 32), L(m + 32) + L(m + 36), e, a + VSIZOFMEM))
			return;
	for (i = 0, lo = e; i < ns; lo += ss[i++]) {
		hr_seg[i * 3] = lo;
		hr_seg[i * 3 + 1] = sb[i];
		hr_seg[i * 3 + 2] = ss[i];
		if (sb[i] < hr_blo)
			hr_blo = sb[i];
		if (sb[i] + ss[i] > hr_bhi)
			hr_bhi = sb[i] + ss[i];
	}
	hr_plo = e;
	hr_phi = e + tot;
	hr_n = ns;
	/* boot modules, then the tables, at the top of the last extent */
	lo = sb[ns - 1];
	hi = lo + ss[ns - 1];
	k = dlm_bootpack(lo, hi, 1) & ~(HR_PG - 1);
	sz = ((e + tot - 1) / HR_PTR - e / HR_PTR + 1) * 512 + ((e + tot - 1) / HR_LEAF - e / HR_LEAF + 1) * 256 +
	    ((a + VSIZOFMEM - 1) / HR_PTR - a / HR_PTR + 1) * 512 + ((a + VSIZOFMEM - 1) / HR_LEAF - a / HR_LEAF + 1) * 256 + 256;
	t = (k - sz) & ~(HR_PG - 1);
	hr_ptbus = t;
	/* supervisor, noncacheable, used, modified, resident */
	t = hr_tables(t, e, tot, 0UL, 0xF9UL);
	/* supervisor, used, modified, resident, the cache mode of RAM */
	(void)hr_tables(t, a, VSIZOFMEM, MAINSTORE, 0x99UL | hat_cm_ram);
	dlm_bootrebase(lo, hi, hr_tokern(lo) - lo);
	kh_alias = a - MAINSTORE;
	hr_alva = a;
	hr_e = e;
	VSIZOFMEM = hr_tokern(hr_ptbus) - MAINSTORE;
}

/*
 * MMU on, before the VM starts: the root's entries for the pseudo range
 * and the cached view, then the transparent windows narrowed to [0, E).
 */
void
amiga_hrmap()
{
	unsigned long *root = (unsigned long *)kroot040, t, v, tt;
	int i;

	if (hr_e == 0)
		return;
	__asm__ __volatile__(".word 0xf478" : : : "memory");	/* cpusha dc */
	t = hr_ptbus;
	for (i = hr_e / HR_PTR; i < HR_TOP / HR_PTR; i++)
		root[i] = 0;
	for (v = hr_e & ~(HR_PTR - 1); v < hr_phi; v += HR_PTR, t += 512)
		root[v / HR_PTR] = t | 2;
	t = (t + ((hr_phi - 1) / HR_LEAF - hr_e / HR_LEAF + 1) * 256 + 511) & ~511UL;
	for (v = hr_alva; v < hr_alva + hr_e - MAINSTORE; v += HR_PTR, t += 512)
		root[v / HR_PTR] = t | 2;
	tt = ((hr_e >> 24) - 1) << 16;
	__asm__ __volatile__("mov.l %0,%%d0; .word 0x4e7b,0x0006"	/* movec %d0,%dtt0 */
	    : : "g" (tt | 0xA060) : "d0");
	__asm__ __volatile__("mov.l %0,%%d0; .word 0x4e7b,0x0004"	/* movec %d0,%itt0 */
	    : : "g" (tt | 0xA000) : "d0");
	__asm__ __volatile__(".word 0xf518" : : : "memory");	/* pflusha */
}

/*
 * Runs after config(), interrupts still masked.  RAM outside the kernel's
 * region joins it if it can, else memory is capped, and boards AmigaOS
 * drivers left interrupting are silenced: nothing would acknowledge them
 * before their own driver starts.
 */
void
amiga_quiesce()
{
	__volatile__ unsigned char *b;
	char *a;
	long n;
	unsigned long k;

	highram();
	if (hr_e == 0) {
		if (VSIZOFMEM > MAXMEM)
			VSIZOFMEM = MAXMEM;
		k = dlm_bootpack((unsigned long)end, MAINSTORE + VSIZOFMEM, 1);
		if (k < MAINSTORE + VSIZOFMEM)		/* boot modules above */
			VSIZOFMEM = (k & ~0xFFFUL) - MAINSTORE;
	}
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
	*pap = hr_tobus((unsigned long)p);
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

/* the low gigabyte (to E with RAM above) and the top one are mapped one to one, noncacheable */
char *
plat_iomap(pa, size)
unsigned long pa, size;
{
	unsigned long tt = hr_e ? hr_e : 0x40000000;

	if (pa < tt && size <= tt - pa || pa >= 0xC0000000)
		return (char *)pa;
	if (pa < 0x40000000)
		return dev_kvmap(pa, size, 0x40, 0);
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
	if (va >= 0x80000000 && va < 0xC0000000)	/* Zorro III window */
		return va - IOWIN;
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

/*
 * Bytes of RAM or board space from a, 0 in a hole, where an access never
 * ends without a bus timeout.  The kernel window is left to the fault path.
 */
long
plat_kmemlen(a)
unsigned long a;
{
	char *m;
	unsigned long lo, hi;
	int i;

	if (a >= 0x40000000 && a < 0x80000000)
		return -1;
	if (hr_e && a >= hr_e && a < 0x40000000)
		return a < hr_phi ? hr_phi - a : 0;
	for (i = 0, m = bootinfo + NAUTO * CDSIZE; i < 16; i++, m += 32) {
		lo = W(m + 14) & 2 ? 0 : L(m + 20);		/* chip RAM from 0 */
		hi = L(m + 24);
		if (a >= lo && a < hi)
			return hi - a;
	}
	for (i = 0, m = bootinfo; i < NAUTO; i++, m += CDSIZE)
		if (a >= L(m + 32) && a - L(m + 32) < L(m + 36))
			return L(m + 32) + L(m + 36) - a;
	return 0;
}
