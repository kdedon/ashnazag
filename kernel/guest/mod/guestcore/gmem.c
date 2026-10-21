#include "kinc.h"
#include "vm/page.h"
#include "sys/tuneable.h"
#include "guest_mem.h"
#include "gmem_impl.h"

extern struct modwrapper guestcore_wrapper;
unsigned long guest_ram_objects, guest_ram_pages, guest_ram_reserved;
extern int availrmem;
extern u_int pages_pp_kernel;
extern struct tune tune;
extern void pagezero(), ppcopy(), dlm_cacheflush();
__asm__(".weak hat_cm_ram");
extern long hat_cm_ram;
static long *__volatile__ gm_hat4k = &hat_cm_ram;


static unsigned long
gm_pfn(pp)
page_t *pp;
{
	return pages_base + (pp - pages);
}

static void
gm_put(pp)
page_t *pp;
{
	if (--pp->p_keepcnt == 0)
		page_abort(pp);
	pages_pp_kernel--;
	guest_ram_pages--;
}

/* RAM is charged to availrmem as pages arrive, not when the object is made. */
static int
gm_charge(m, n)
struct guest_ram *m;
unsigned long n;
{
	if (availrmem - (int)n < tune.t_minarmem)
		return 0;
	availrmem -= n;
	m->reserved += n;
	guest_ram_reserved += n;
	return 1;
}

static void
gm_uncharge(m, n)
struct guest_ram *m;
unsigned long n;
{
	availrmem += n;
	m->reserved -= n;
	guest_ram_reserved -= n;
}

/* DMA pages remain fixed until the RAM object is released. */
static page_t *
gm_run(m, n, limit)
struct guest_ram *m;
unsigned long n, limit;
{
	page_t *p, *lo, *next;
	unsigned long i;

	pages_pp_kernel += n;
	guest_ram_pages += n;
	p = page_get(n << m->shift, P_NOSLEEP | P_PHYSCONTIG);
	if (!p) {
		pages_pp_kernel -= n;
		guest_ram_pages -= n;
		return 0;
	}
	lo = p;
	for (i = 1; i < n; i++) {
		p = p->p_next;
		if (p < lo)
			lo = p;
	}
	if (limit && ((gm_pfn(lo) + n) << m->shift) > limit) {
		for (i = 0, p = lo; i < n; i++, p = next) {
			next = p + 1;
			gm_put(p);
		}
		return 0;
	}
	return lo;
}

void
guest_ram_free(m)
struct guest_ram *m;
{
	unsigned long i;

	if (!m || --m->refs)
		return;
	for (i = 0; i < m->n; i++)
		if (m->page[i])
			gm_put(m->page[i]);
	kmem_free(m->page, m->n * sizeof *m->page);
	kmem_free(m->dma, m->n);
	gm_uncharge(m, m->reserved);
	if (m->held) {
		guest_ram_objects--;
		mod_rele(&guestcore_wrapper);
	}
	kmem_free(m, sizeof *m);
}

int
guest_ram_new(size, flags, limit, out)
unsigned long size, limit;
int flags;
struct guest_ram **out;
{
	struct guest_ram *m;
	page_t *p;
	unsigned long i;
	unsigned int shift = gm_hat4k ? 12 : 11;

	*out = 0;
	if (!size || size > 0x4000000UL || (size & ((1UL << shift) - 1)) ||
	    (flags & ~(GMR_CONTIG | GMR_WT)))
		return EINVAL;
	m = (struct guest_ram *)kmem_zalloc(sizeof *m, KM_SLEEP);
	m->size = size;
	m->refs = 1;
	m->wt = (flags & GMR_WT) != 0;
	m->shift = shift;
	m->n = size >> shift;
	m->page = (page_t **)kmem_zalloc(m->n * sizeof *m->page, KM_SLEEP);
	m->dma = (unsigned char *)kmem_zalloc(m->n, KM_SLEEP);
	/* the whole size must fit now; paged RAM is charged as it faults in */
	if (availrmem - (int)m->n < tune.t_minarmem ||
	    ((flags & GMR_CONTIG) && !gm_charge(m, m->n))) {
		guest_ram_free(m);
		return ENOMEM;
	}
	if (flags & GMR_CONTIG) {
		if (!(p = gm_run(m, m->n, limit))) {
			guest_ram_free(m);
			return ENOMEM;
		}
		for (i = 0; i < m->n; i++) {
			m->page[i] = p + i;
			m->dma[i] = 1;
			pagezero(p + i, 0, 1UL << shift);
		}
	}
	mod_hold(&guestcore_wrapper);
	m->held = 1;
	guest_ram_objects++;
	*out = m;
	return 0;
}

int
guest_ram_frame(m, off)
struct guest_ram *m;
unsigned long off;
{
	page_t *p;
	unsigned long i;

	if (!m || off >= m->size)
		return -1;
	i = off >> m->shift;
	if (!m->page[i]) {
		if (!gm_charge(m, 1UL))
			return -1;
		p = page_get(1UL << m->shift, P_CANWAIT);
		if (!p) {
			gm_uncharge(m, 1UL);
			return -1;
		}
		pages_pp_kernel++;
		guest_ram_pages++;
		/* Another mapping may have installed this frame while page_get slept. */
		if (m->page[i]) {
			gm_put(p);
			gm_uncharge(m, 1UL);
			return (int)gm_pfn(m->page[i]);
		}
		pagezero(p, 0, 1UL << m->shift);
		m->page[i] = p;
	}
	return (int)gm_pfn(m->page[i]);
}

unsigned long
guest_ram_size(m)
struct guest_ram *m;
{
	return m ? m->size : 0;
}

void
guest_ram_invalidator(m, fn, arg)
struct guest_ram *m;
void (*fn)();
char *arg;
{
	m->invalidate = fn;
	m->arg = arg;
}

/* Replace only the requested DMA span; ordinary RAM keeps scattered pages. */
int
guest_ram_phys(m, off, len, limit, out)
struct guest_ram *m;
unsigned long off, len, limit, *out;
{
	page_t *p, *old;
	unsigned long first, last, n, i, pa, extra = 0;
	int same = 1;

	*out = 0;
	if (!m || !len || off >= m->size || len > m->size - off)
		return EINVAL;
	first = off >> m->shift;
	last = (off + len - 1) >> m->shift;
	n = last - first + 1;
	p = m->page[first];
	for (i = first; i <= last; i++)
		if (!p || m->page[i] != p + i - first)
			same = 0;
	if (same) {
		pa = (gm_pfn(p) << m->shift) + (off & ((1UL << m->shift) - 1));
		if (!limit || (pa < limit && len <= limit - pa)) {
			for (i = first; i <= last && m->dma[i]; i++)
				;
			if (i <= last) {
				if (m->softlocks)
					return EBUSY;
				gm_unload(m);
				dlm_cacheflush();
			}
			for (i = first; i <= last; i++)
				m->dma[i] = 1;
			*out = pa;
			return 0;
		}
	}
	for (i = first; i <= last; i++)
		if (m->dma[i])
			return EBUSY;
	if (m->softlocks)
		return EBUSY;
	for (i = first; i <= last; i++)
		if (m->page[i])
			extra++;
	if (!gm_charge(m, n))
		return ENOMEM;
	if (!(p = gm_run(m, n, limit))) {
		gm_uncharge(m, n);
		return ENOMEM;
	}
	gm_unload(m);
	if (m->invalidate)
		(*m->invalidate)(m->arg, first << m->shift, n << m->shift);
	dlm_cacheflush();
	for (i = 0; i < n; i++) {
		old = m->page[first + i];
		if (old)
			ppcopy(old, p + i);
		else
			pagezero(p + i, 0, 1UL << m->shift);
		m->page[first + i] = p + i;
		m->dma[first + i] = 1;
		if (old)
			gm_put(old);
	}
	gm_uncharge(m, extra);
	dlm_cacheflush();
	*out = (gm_pfn(p) << m->shift) + (off & ((1UL << m->shift) - 1));
	return 0;
}
