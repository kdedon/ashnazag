/*
 * RAM chunks besides the kernel's.  The VM pages one range,
 * [pages_base, pages_end), so it is widened to cover them: a chunk at
 * address 0 below the kernel, or a vr_free() range, makes the page array
 * start at its lowest page, and
 * chunks above raise maxclick once mlsetup has cleared its memory.
 * memialloc then frees only the chunks; page structures for the holes
 * and the kernel stay locked and unused (60 bytes per page).
 *
 *   0        kernel chunk          hole        chunk above
 *   |-- lo --|== kernel ==|-- VM --|..........|-- VM --|
 *   pages_base                                  maxclick
 *
 * config() calls vr_init(), vr_add() per chunk, then vr_setup().
 * vr_free() gives the VM an unused range inside the kernel image.
 * Everything here runs before BSS is cleared, so nothing lives there.
 */

#define VR_MAX		4
#define PAGESZ		60		/* sizeof (page_t) */
#define P_LOCK		0x80		/* first byte of page_t */
#define P_FREE		0x20
#define P_PHYSCONTIG	2		/* page_get() */

unsigned long vr_lo[VR_MAX] = { 1 };	/* clicks */
unsigned long vr_hi[VR_MAX] = { 1 };
int vr_n = 1;
int vr_shift = 1;
int vr_low = 1;			/* a chunk sits below the kernel's */
unsigned long vr_kend = 1;	/* end of the kernel's chunk, clicks */
unsigned long vr_flo = 1, vr_fhi = 1;	/* vr_free() range, clicks */

extern unsigned long MAINSTORE, VSIZOFMEM;
extern int maxclick, physmem, maxmem, pages_base, pages_end;
extern char *pages, *epages;
extern unsigned short __amix_kvm_init[];
extern void __amix_memialloc();
extern char *page_get();
static int vr_patch();

void
vr_init(shift)
int shift;
{
	vr_n = 0;
	vr_low = 0;
	vr_flo = vr_fhi = 0;
	vr_shift = shift;
	vr_kend = (MAINSTORE + VSIZOFMEM) >> shift;
}

/* [lo, hi) of the chunk at base joins the VM; below the kernel only at 0 */
int
vr_add(base, lo, hi)
unsigned long base, lo, hi;
{
	lo = (lo + (1UL << vr_shift) - 1) >> vr_shift;
	hi >>= vr_shift;
	if (vr_n == VR_MAX || lo >= hi)
		return 0;
	if (hi <= MAINSTORE >> vr_shift) {
		if (base != 0 || !vr_patch())
			return 0;
		vr_low = 1;
	} else if (lo < vr_kend)
		return 0;
	vr_lo[vr_n] = lo;
	vr_hi[vr_n] = hi;
	vr_n++;
	return 1;
}

/*
 * kvm_init sizes the page array as (maxclick - first) entries: subl
 * %d5,%d1; moveq #60,%d2; mulsl %d1,%d2.  With a chunk below, drop the
 * subtraction so the array covers pages from 0.  Returns 0 if the code
 * is not found or cannot be used, after dropping the chunks below.
 */
static int
vr_patch()
{
	register unsigned short *p;

	/* the 2 KB-page HAT finds the kernel's page tables only outside the array */
	if (vr_shift < 12)
		return 0;
	for (p = __amix_kvm_init; p < __amix_kvm_init + 0x200; p++)
		if ((p[0] == 0x9285 || p[0] == 0x4e71) && p[1] == 0x743c &&
		    p[2] == 0x4c01 && p[3] == 0x2802) {
			p[0] = 0x4e71;
			return 1;
		}
	return 0;
}

int
vr_setup()
{
	register int i, j;

	if (!vr_low || vr_patch())
		return 1;
	for (i = j = 0; i < vr_n; i++)
		if (vr_lo[i] >= vr_kend) {
			vr_lo[j] = vr_lo[i];
			vr_hi[j++] = vr_hi[i];
		}
	vr_n = j;
	vr_low = 0;
	return 0;
}

/* [lo, hi) of the kernel image, bytes, is free memory once the VM starts */
void
vr_free(lo, hi)
unsigned long lo, hi;
{
	lo = (lo + (1UL << vr_shift) - 1) >> vr_shift;
	hi >>= vr_shift;
	if (lo < hi && vr_patch()) {
		vr_low = 1;
		vr_flo = lo;
		vr_fhi = hi;
	}
}

#ifdef VR_UP
extern int __amix_sysseginit();
extern void bzero();

/* mlsetup has cleared [first, maxclick): clear the chunks above, extend */
int
sysseginit(v)
int v;
{
	register int i, r;

	r = __amix_sysseginit(v);
	for (i = 0; i < vr_n; i++)
		if (vr_lo[i] >= vr_kend && vr_hi[i] > maxclick) {
			bzero((char *)(vr_lo[i] << vr_shift), (vr_hi[i] - vr_lo[i]) << vr_shift);
			maxclick = vr_hi[i];
		}
	VSIZOFMEM = ((unsigned long)maxclick << vr_shift) - MAINSTORE;
	return r;
}
#endif

/* Free the kernel chunk past first, then the others; count them. */
void
memialloc(first, last)
int first, last;
{
	register char *p;
	register int i, n, lo;

	if (vr_low) {
		p = epages;
		pages_end = last;
		epages = pages + last * PAGESZ;
		for (; p < epages; p += PAGESZ)
			*p |= P_LOCK;
		/* the HAT takes frames below pages_base for unmanaged ones */
		lo = vr_flo < vr_fhi ? vr_flo : first;
		for (i = 0; i < vr_n; i++)
			if (vr_lo[i] < lo)
				lo = vr_lo[i];
		pages_base = lo;
		pages += lo * PAGESZ;
	}
	n = last < vr_kend ? last : vr_kend;
	__amix_memialloc(first, n);
	n -= first;
	if (vr_flo < vr_fhi) {
		__amix_memialloc(vr_flo, vr_fhi);
		n += vr_fhi - vr_flo;
	}
	for (i = 0; i < vr_n; i++)
		if (vr_hi[i] <= last) {
			__amix_memialloc(vr_lo[i], vr_hi[i]);
			n += vr_hi[i] - vr_lo[i];
			physmem += vr_hi[i] - vr_lo[i];
		}
	maxmem = n;
}

/*
 * size bytes of contiguous pages below lim, taken from the VM for good:
 * their physical address, also the kernel's, or 0.  page_get() takes
 * the lowest free run, so find it first and check it is below lim.
 */
unsigned long
vr_contig(size, lim)
unsigned long size, lim;
{
	register char *p;
	register unsigned long n, run;
	unsigned long pa;
	int s;

	n = (size + (1UL << vr_shift) - 1) >> vr_shift;
	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" ((s & 0x700) < 0x400 ? (s & ~0x700) | 0x400 : s) : "memory");
	for (run = 0, p = pages; p < epages && run < n; p += PAGESZ)
		run = *p & P_FREE ? run + 1 : 0;
	pa = (unsigned long)(pages_base + (p - pages) / PAGESZ - n) << vr_shift;
	if (run < n || pa + (n << vr_shift) > lim || page_get(n << vr_shift, P_PHYSCONTIG) == 0)
		pa = 0;
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
	return pa;
}
