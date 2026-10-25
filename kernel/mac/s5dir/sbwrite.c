/*
 * fbwritei() for s5 superblock writes, its only callers.
 *
 * The superblock goes to disk as the whole device page holding it.  With
 * 512- and 1024-byte blocks that page also holds the first inode blocks,
 * which s5 keeps in the buffer cache, so the page's copy of them may be
 * old: it was read before a later inode write, or kept from an earlier
 * superblock write.  Before the page is written, each of those blocks is
 * taken from the buffer cache, which holds the buffer busy until the page
 * is on disk, so no inode write can slip in between.
 *
 *	page 0, 1 KB blocks:	| boot+super | unused | inodes 1-16 | 17-32 |
 *				  written      buffer    buffer        buffer
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/buf.h"
#include "sys/fbuf.h"
#include "sys/vnode.h"
#include "sys/fs/s5param.h"
#include "sys/fs/s5filsys.h"
#include "vm/seg.h"
#include "vm/seg_map.h"

#ifndef PGSZ
#define PGSZ	4096		/* the 68040 kernel's page size */
#endif
#define SBOFF	512		/* superblock offset on the device */
#define FIRSTI	2		/* first inode block */

extern int	__amix_fbwritei();
extern struct buf *bread();
extern void	brelse(), bcopy();

int
fbwritei(fbp)
struct fbuf	*fbp;
{
	struct filsys	*fp = (struct filsys *)fbp->fb_addr;
	struct seg	*seg = segkmap;
	struct smap	*smp;
	struct buf	*bp[PGSZ / 512];
	u_int		va = (u_int)fbp->fb_addr;
	caddr_t		pg;
	int		bsize, n, b, error;
	dev_t		dev;

	if (fbp->fb_count != sizeof (struct filsys) || (va & (PGSZ - 1)) != SBOFF ||
	    seg == 0 || va < (u_int)seg->s_base || va >= (u_int)seg->s_base + seg->s_size ||
	    fp->s_magic != FsMAGIC || fp->s_type < Fs1b || fp->s_type > Fs2b)
		return __amix_fbwritei(fbp);
	smp = &((struct segmap_data *)seg->s_data)->smd_sm[(va - (u_int)seg->s_base) >> MAXBSHIFT];
	if (smp->sm_vp == 0 || smp->sm_off + (va & MAXBOFFSET) != SBOFF)
		return __amix_fbwritei(fbp);
	dev = smp->sm_vp->v_rdev;
	bsize = 512 << (fp->s_type - Fs1b);
	n = PGSZ / bsize;
	pg = (caddr_t)(va & ~(PGSZ - 1));
	for (b = FIRSTI; b < n; b++) {
		bp[b] = bread(dev, (daddr_t)b, bsize);
		if ((bp[b]->b_flags & B_ERROR) == 0)
			bcopy(bp[b]->b_un.b_addr, pg + b * bsize, bsize);
	}
	error = __amix_fbwritei(fbp);
	for (b = FIRSTI; b < n; b++)
		brelse(bp[b]);
	return error;
}
