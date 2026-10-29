/*
 * The kernel segment's page table holds bus frames; the page array is
 * indexed by kernel frames (hr_tobus, hr_tokern).  segkmem_alloc,
 * segkmem_free, segkmem_mapin and segkmem_mapout as stock otherwise.
 * 68040/68060 kernels only.  K&R C.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/cmn_err.h"
#include "vm/seg.h"
#include "vm/page.h"

extern page_t *page_get(), *page_numtookpp();
extern page_t *pages;
extern u_int pages_base;
extern void page_sub(), page_abort(), wakeprocs(), flushmmu();
extern int hat_vtokp_prot();
extern u_long hr_tobus(), hr_tokern();
extern struct as kas;

#define	KS_PG		4096
#define	KS_FRAME	0xfffff000UL
#define	KS_WANT		0x40		/* p_want, in the first byte */
#define	KS_GONE		0x08		/* p_gone */

static u_long *
ks_pte(seg, addr, who)
	struct seg *seg;
	addr_t addr;
	char *who;
{
	if (seg->s_data == 0)
		cmn_err(CE_PANIC, "%s: invalid for non-paged segment", who);
	return (u_long *)seg->s_data + (u_long)(addr - seg->s_base) / KS_PG;
}

/* the page of a kernel PTE, or 0 */
static page_t *
ks_page(pte)
	u_long pte;
{
	return page_numtookpp(hr_tokern(pte & KS_FRAME) / KS_PG);
}

/* drop one keep of pp; the last one frees it unless the page is named */
static void
ks_rele(pp)
	register page_t *pp;
{
	if (--pp->p_keepcnt == 0)
		while (*(u_char *)pp & KS_WANT) {
			wakeprocs((caddr_t)pp, 1);
			*(u_char *)pp &= ~KS_WANT;
		}
	if (pp->p_keepcnt == 0 && ((*(u_char *)pp & KS_GONE) || pp->p_vnode == 0))
		page_abort(pp);
}

int
segkmem_alloc(seg, addr, len, canwait)
	struct seg *seg;
	addr_t addr;
	u_int len;
	int canwait;
{
	register u_long *pte;
	page_t *pl, *pp;

	pte = ks_pte(seg, addr, "segkmem_alloc");
	if ((pl = page_get(len, canwait ? 1 : 0)) == 0)
		return 0;
	while (pl) {
		pp = pl;
		page_sub(&pl, pp);
		*pte++ = hr_tobus(((u_long)(pp - pages) + pages_base) * KS_PG) | 1;
		flushmmu(addr, 1);
		addr += KS_PG;
	}
	return 1;
}

void
segkmem_free(seg, addr, len)
	struct seg *seg;
	addr_t addr;
	u_int len;
{
	register u_long *pte;
	register int n;
	page_t *pp;
	u_long v;

	pte = ks_pte(seg, addr, "segkmem_free");
	for (n = len; n > 0; n -= KS_PG) {
		v = *pte;
		*pte++ = 0;
		if ((pp = ks_page(v)) == 0)
			cmn_err(CE_PANIC, "segkmem_free");
		ks_rele(pp);
	}
	flushmmu(addr, (len + KS_PG - 1) / KS_PG);
}

void
segkmem_mapin(seg, addr, len, prot, pfpte, flags)
	struct seg *seg;
	addr_t addr;
	u_int len, prot;
	u_long pfpte;
	int flags;
{
	register u_long *pte, t;
	page_t *pp, *opp;
	u_long o;

	pte = ks_pte(seg, addr, "segkmem_mapin");
	if (prot == 0)
		cmn_err(CE_PANIC, "segkmem_mapin -- invalid ptes");
	t = (pfpte & ~4UL) | (hat_vtokp_prot(prot) & 1) << 2 | 1;
	for (; len; len -= KS_PG, addr += KS_PG, pte++, t += KS_PG) {
		pp = page_numtookpp(t / KS_PG);
		if (!(flags & 2) && (*pte & 1)) {
			o = hr_tokern(*pte & KS_FRAME) | (*pte & ~KS_FRAME);
			if ((o & 0xfffff001) == (t & 0xfffff001))
				continue;
			if ((opp = ks_page(*pte)) != 0)
				ks_rele(opp);
		}
		if (pp)
			pp->p_keepcnt++;
		*pte = hr_tobus(t & KS_FRAME) | (t & ~KS_FRAME);
		flushmmu(addr, 1);
	}
}

void
segkmem_mapout(seg, addr, len)
	struct seg *seg;
	addr_t addr;
	u_int len;
{
	register u_long *pte;
	register u_int n;
	page_t *pp;

	if (seg->s_as != &kas)
		cmn_err(CE_PANIC, "segkmem_mapout: bad as");
	pte = ks_pte(seg, addr, "segkmem_mapout");
	for (n = len; n; n -= KS_PG, pte++)
		if (*pte & 1) {
			*pte &= ~1UL;
			if ((pp = ks_page(*pte)) != 0)
				ks_rele(pp);
		}
	flushmmu(addr, (len + KS_PG - 1) / KS_PG);
}
