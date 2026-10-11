/*
 * atafsflush.c -- the fsflush daemon, as stock, but the page scan and the
 * attribute sync run at the caller's level, as pageout's scan does.  At
 * splhi the scan masks the IKBD's ACIA for several ms, and it holds one
 * byte: keys and mouse bytes were lost.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/immu.h"
#include "sys/signal.h"
#include "sys/fs/s5dir.h"
#include "sys/psw.h"
#include "sys/pcb.h"
#include "sys/user.h"
#include "sys/proc.h"
#include "sys/buf.h"
#include "sys/var.h"
#include "sys/tuneable.h"
#include "sys/vnode.h"
#include "sys/vfs.h"
#include "sys/disp.h"
#include "vm/page.h"

extern int dopageflush, doiflush, lbolt;
extern int ata_spltty(), hat_pagesync(), sleep();
extern void ata_splx();
extern struct var v;
extern struct tune tune;

void
fsflush()
{
	register struct buf *bp;
	register page_t *pp = pages;
	register struct vnode *vp;
	unsigned long i, npages, icount = 0, iwait;
	int s, s2, autoup;

	autoup = v.v_autoup * HZ;
	npages = (epages - pages) * tune.t_fsflushr / v.v_autoup;
	iwait = v.v_autoup / tune.t_fsflushr;
	for (;;) {
		s = ata_spltty();
		for (bp = bfreelist.av_forw; bp != &bfreelist; bp = bp->av_forw)
			if ((bp->b_flags & B_DELWRI) && lbolt - bp->b_start >= autoup)
				break;
		if (bp != &bfreelist) {
			bp->b_flags |= B_ASYNC;
			s2 = ata_spltty();
			bp->av_back->av_forw = bp->av_forw;
			bp->av_forw->av_back = bp->av_back;
			bp->b_flags |= B_BUSY;
			bfreelist.b_bcount--;
			ata_splx(s2);
			bwrite(bp);
			ata_splx(s);
			if (kprunrun)
				preempt();
			continue;
		}
		ata_splx(s);
		if (dopageflush)
			for (i = 0; i <= npages; i++) {
				vp = pp->p_vnode;
				if (vp && !pp->p_free && !pp->p_lock && !pp->p_intrans &&
				    !pp->p_lckcnt && !pp->p_cowcnt && !pp->p_keepcnt &&
				    !(vp->v_flag & VISSWAP) && vp->v_type != VCHR) {
					hat_pagesync(pp);
					if (pp->p_mod)
						VOP_PUTPAGE(vp, pp->p_offset, PAGESIZE, B_ASYNC,
						    (struct cred *)0);
				}
				if (++pp >= epages)
					pp = pages;
			}
		if (doiflush && icount++ >= iwait) {
			icount = 0;
			for (i = 1; i < nfstype; i++)
				(*vfssw[i].vsw_vfsops->vfs_sync)((struct vfs *)0, SYNC_ATTR, u.u_cred);
		}
		sleep((caddr_t)fsflush, PRIBIO);
	}
}
