/*
 * uiomove() for writes into the kernel's file window (segkmap): after a
 * successful copy, mark every page it wrote modified.
 *
 * writei() and rdwri() copy into cached file pages through segkmap and
 * leave the dirty state to the MMU's modified bit.  A store through a
 * translation entry loaded by a read may leave that bit clear; the page
 * then looks clean to VOP_PUTPAGE and s5 directory updates never reach
 * the disk.
 * A failed copy marks nothing: a page created for a full overwrite holds
 * stale memory until the copy fills it.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/uio.h"
#include "sys/vnode.h"
#include "vm/seg.h"
#include "vm/seg_map.h"

#define PGSZ	4096		/* the 68040 kernel's page size */
#define PGMOD	0x04		/* p_mod in the first byte of a page_t */

extern int	__amix_uiomove();
extern char	*page_exists();

int
uiomove(cp, n, rw, uiop)
caddr_t		cp;
long		n;
enum uio_rw	rw;
struct uio	*uiop;
{
	int		error, resid;
	struct seg	*seg = segkmap;
	struct smap	*smp;
	u_int		va, end, lim;
	char		*pp;

	va = (u_int)cp;
	if (rw != UIO_WRITE || n <= 0 || seg == 0 ||
	    va < (u_int)seg->s_base || va >= (lim = (u_int)seg->s_base + seg->s_size))
		return __amix_uiomove(cp, n, rw, uiop);
	resid = uiop->uio_resid;
	error = __amix_uiomove(cp, n, rw, uiop);
	if (error)
		return error;
	end = va + (resid - uiop->uio_resid);
	if (end > lim)
		end = lim;
	for (va &= ~(PGSZ - 1); va < end; va += PGSZ) {
		smp = &((struct segmap_data *)seg->s_data)->smd_sm[
		    (va - (u_int)seg->s_base) >> MAXBSHIFT];
		if (smp->sm_vp == 0)
			continue;
		pp = page_exists(smp->sm_vp, smp->sm_off + (va & MAXBOFFSET));
		if (pp)
			*(volatile u_char *)pp |= PGMOD;	/* one orib: atomic against interrupts */
	}
	return 0;
}
