/*
 * The kernel heap grows on demand.  sptalloc takes kernel virtual
 * space from one fixed 4 MB arena; once that is short, page-backed
 * requests go to 4 MB chunks added above the u-areas (free kernel
 * virtual space up to 0x7fffffff), each with its own
 * leaf page tables in one page taken when the chunk is added.
 *
 *	arena (fixed)   heap, buffers, device windows; the heap leaves
 *	                KH_KEEP pages for paging I/O mappings
 *	chunk 0..n-1    heap only, added when the arena is short
 *
 * 68040/68060 kernels only (region-1 pointer table kptr040).  K&R C.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/systm.h"
#include "sys/inline.h"
#include "sys/map.h"
#include "sys/kmem.h"
#include "sys/tuneable.h"
#include "sys/cmn_err.h"
#include "vm/seg.h"
#include "vm/page.h"

extern caddr_t __vm_sptalloc();
extern _VOID *__vm_kmem_alloc();
extern void __vm_sptfree(), flushmmu(), bzero();
extern int segkmem_alloc(), segkmem_mapout();
extern page_t *page_get();
extern page_t *pages;
extern u_int pages_base;
extern u_long *kptr040;
extern struct seg kvseg, *segu;

#define	KH_PG		4096
#define	KH_NPG		1024			/* pages per chunk */
#define	KH_SIZE		(KH_NPG * KH_PG)
#define	KH_MAX		32			/* chunks: 128 MB */
#define	KH_PTSEG	(256 * 1024)		/* VA per pointer descriptor */
#define	KH_KEEP		256			/* arena pages kept for buffer and device mappings */

struct kh_chunk {
	struct seg c_seg;
	u_char	c_used[KH_NPG / 8];
	u_int	c_free;
};

static struct kh_chunk kh_chunk[KH_MAX];
static int kh_n;
static int kh_wait;		/* sleepers in kmem_alloc */
static u_long kh_base;

/* first fit of n pages in c, or -1 */
static int
kh_fit(c, n)
	register struct kh_chunk *c;
	register int n;
{
	register int i, run = 0;

	if (c->c_free < n)
		return -1;
	for (i = 0; i < KH_NPG; i++) {
		if (!(i & 7) && c->c_used[i >> 3] == 0xff) {
			run = 0;
			i += 7;
		} else if (c->c_used[i >> 3] & (1 << (i & 7)))
			run = 0;
		else if (++run == n)
			return i - n + 1;
	}
	return -1;
}

static void
kh_mark(c, i, n, set)
	register struct kh_chunk *c;
	register int i, n;
	int set;
{
	for (; n > 0; n--, i++)
		if (set)
			c->c_used[i >> 3] |= 1 << (i & 7);
		else
			c->c_used[i >> 3] &= ~(1 << (i & 7));
}

/* a new chunk, its leaf tables in one fresh page; called at splhi */
static struct kh_chunk *
kh_add()
{
	register struct kh_chunk *c;
	register u_long *pd;
	page_t *pp, *pl;
	u_long va, pt;
	int i;

	if (kh_n == KH_MAX)
		return 0;
	if (kh_base == 0)
		kh_base = ((u_long)segu->s_base + segu->s_size + KH_SIZE - 1) & ~(KH_SIZE - 1);
	va = kh_base + (u_long)kh_n * KH_SIZE;
	if (va + KH_SIZE - 1 > 0x7fffffff)
		return 0;
	pd = &kptr040[(va >> 18) - 4096];
	for (i = 0; i < KH_SIZE / KH_PTSEG; i++)
		if (pd[i])
			return 0;
	if ((pl = page_get(KH_PG, 0)) == 0)
		return 0;
	pp = pl;
	page_sub(&pl, pp);
	pt = page_pptonum(pp) * KH_PG;
	/* no dirty line of the page's last user may land on the tables */
	flushmmu(0, 0);
	bzero((caddr_t)pt, KH_PG);
	c = &kh_chunk[kh_n];
	c->c_seg = kvseg;
	c->c_seg.s_base = (addr_t)va;
	c->c_seg.s_size = KH_SIZE;
	c->c_seg.s_data = (_VOID *)pt;
	c->c_free = KH_NPG;
	/* 256-byte leaf tables of 64 entries */
	for (i = 0; i < KH_SIZE / KH_PTSEG; i++)
		pd[i] = (pt + i * 256) | 2;
	flushmmu(0, 0);
	kh_n++;
	return c;
}

/* free pages of the fixed arena */
static u_long
kh_room()
{
	register struct map *m;
	register u_long n = 0;
	int s = splhi();

	for (m = mapstart(sptmap); m->m_size; m++)
		n += m->m_size;
	splx(s);
	return n;
}

caddr_t
sptalloc(npages, mode, base, flag)
	int npages, mode, base, flag;
{
	register struct kh_chunk *c;
	caddr_t va;
	int i, s;

	if (base || npages <= 0 || npages > KH_NPG)
		return __vm_sptalloc(npages, mode, base, flag);
	s = splhi();
	for (c = kh_chunk; c < &kh_chunk[kh_n]; c++)
		if ((i = kh_fit(c, npages)) >= 0)
			break;
	if (c == &kh_chunk[kh_n]) {
		splx(s);
		if (kh_room() >= npages + KH_KEEP &&
		    (va = __vm_sptalloc(npages, mode, base, flag)) != 0)
			return va;
		s = splhi();
		c = kh_add();
		if (c == 0 || (i = kh_fit(c, npages)) < 0) {
			splx(s);
			/* a caller that may sleep waits for the arena */
			return flag & 1 ? 0 : __vm_sptalloc(npages, mode, base, flag);
		}
	}
	kh_mark(c, i, npages, 1);
	c->c_free -= npages;
	splx(s);
	va = (caddr_t)c->c_seg.s_base + i * KH_PG;
	if (!segkmem_alloc(&c->c_seg, va, npages * KH_PG, !(flag & 1))) {
		s = splhi();
		kh_mark(c, i, npages, 0);
		c->c_free += npages;
		splx(s);
		return 0;
	}
	return va;
}

void
sptfree(va, npages, flag)
	caddr_t va;
	int npages, flag;
{
	register struct kh_chunk *c;
	register u_long *pte;
	int i, s;

	for (c = kh_chunk; c < &kh_chunk[kh_n]; c++)
		if (va >= (caddr_t)c->c_seg.s_base && va < (caddr_t)c->c_seg.s_base + KH_SIZE)
			break;
	if (c == &kh_chunk[kh_n]) {
		__vm_sptfree(va, npages, flag);
		if (kh_wait)
			wakeup((caddr_t)&kh_wait);
		return;
	}
	i = (va - (caddr_t)c->c_seg.s_base) / KH_PG;
	if (flag)
		segkmem_mapout(&c->c_seg, va, npages * KH_PG);
	else {
		pte = (u_long *)c->c_seg.s_data + i;
		bzero((caddr_t)pte, npages * sizeof *pte);
		flushmmu(0, 0);
	}
	s = splhi();
	kh_mark(c, i, npages, 0);
	c->c_free += npages;
	splx(s);
	if (kh_wait)
		wakeup((caddr_t)&kh_wait);
}

/*
 * Over a page, a sleeping kmem_alloc that no heap and no RAM could ever
 * hold panics with its size.  Otherwise it waits for a free, retrying at
 * least once a second, where the stock path waits for a free at least
 * as large, which may never come.  Callers may hold locks, so no signal
 * ends the wait.
 */
_VOID *
kmem_alloc(size, flag)
	size_t size;
	int flag;
{
	_VOID *va;
	int n, id;

	if (size <= KH_PG || (flag & KM_NOSLEEP))
		return __vm_kmem_alloc(size, flag);
	n = (size + KH_PG - 1) / KH_PG;
	if (n > KH_NPG || n + tune.t_minarmem > maxmem)
		cmn_err(CE_PANIC, "kmem_alloc: %u bytes never fit", size);
	while ((va = __vm_kmem_alloc(size, KM_NOSLEEP)) == 0) {
		kh_wait++;
		id = timeout((void (*)())wakeup, (caddr_t)&kh_wait, HZ);
		(void)sleep((caddr_t)&kh_wait, PZERO);
		untimeout(id);
		kh_wait--;
	}
	return va;
}
