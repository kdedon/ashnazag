/*
 * atavpfree.c -- free_vp_pages as stock, but at IPL 5: the walk of a
 * vnode's page list ran at splhi, masking the IKBD's ACIA for long enough
 * to lose key bytes under file load.  Pages change at interrupt level only
 * from disk completions, which IPL 5 masks.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/vnode.h"
#include "vm/page.h"

extern int free_pages, freemem, freemem_wait, kprunrun, pagecnt;
extern int page_cachelist_size;
extern page_t *page_cachelist;
extern int ata_splvm(), swap_in_range();
extern void ata_splx(), preempt(), wakeprocs(), cb_vpfree_enter();
extern void page_add();

void
free_vp_pages(vp, off, len)
register struct vnode *vp;
u_int off, len;
{
	register page_t *pp, *first;
	u_int end = off + len;
	int s;

	if (!free_pages || swap_in_range(vp, off, len))
		return;
	if (kprunrun)
		preempt();
	s = ata_splvm();
	if ((pp = first = vp->v_pages) != NULL)
		do {
			if (pp->p_offset < off || pp->p_offset >= end ||
			    pp->p_mod || pp->p_keepcnt || pp->p_mapping ||
			    pp->p_free || pp->p_lckcnt || pp->p_cowcnt)
				continue;
			pp->p_lock = 0;
			while (pp->p_want) {
				pp->p_want = 0;
				wakeprocs((caddr_t)pp, 1);
			}
#ifdef ATA060
			cb_vpfree_enter();
#else
			freemem++;
#endif
			pp->p_free = 1;
			pp->p_ref = 0;
			page_cachelist_size++;
			page_add(&page_cachelist, pp);
			page_cachelist = page_cachelist->p_next;
			pagecnt++;
		} while ((pp = pp->p_vpnext) != first);
	if (freemem_wait && page_cachelist) {
		freemem_wait = 0;
		wakeprocs((caddr_t)&freemem, 1);
	}
	ata_splx(s);
}
