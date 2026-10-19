/*
 * Virtual swap: anonymous memory is reserved against free RAM plus the
 * swap disk, and a page takes a disk block only when it is written out.
 *
 * Every swap area added becomes the backing store of a virtual area
 * (block device VS_ROW, one minor per area); the first one also counts
 * free RAM, and anon_resv keeps reservations within the RAM free now
 * plus the disks.  The kernel names anonymous pages by
 * virtual slot.  The strategy routine maps each virtual page to a disk
 * page, taking a free one on its first write; a page never written
 * reads as zeros.  swap_free gives the disk page back, once a write
 * still in flight on it is done.  Reservations beyond the disks are
 * taken from availrmem, so locked and kernel pages cannot use RAM
 * promised to anonymous memory.
 *
 *	virtual minor 0: | disk pages ...           | RAM part ... |
 *	                     \ map: any free disk page, first write
 *
 * K&R C.
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
#include "sys/cred.h"
#include "sys/systm.h"
#include "sys/inline.h"
#include "sys/errno.h"
#include "sys/buf.h"
#include "sys/conf.h"
#include "sys/file.h"
#include "sys/open.h"
#include "sys/kmem.h"
#include "sys/tuneable.h"
#include "sys/vnode.h"
#include "vm/anon.h"

extern int availrmem, availsmem, physmem;
extern struct tune tune;
extern struct vnode *makespecvp();
extern int __vm_swapadd(), __vm_swapdel(), __vm_swap_free(), __vm_anon_resv(), swap_xlate();
extern void __vm_anon_unresv();
extern void bzero(), biodone(), bp_mapin();
extern int nodev();

#define	VS_ROW		22		/* rows 21-31 are free in the AMIX table */
#define	VS_NAREA	8
#define	VS_NREQ		16
#define	VS_PGSZ		(1 << VS_PGSHIFT)
#define	VS_PGBLK	(1 << (VS_PGSHIFT - 9))
#define	VS_CALL		B_KERNBUF	/* biodone calls b_iodone */
#define	VS_KEEP		(0x200000 >> VS_PGSHIFT)	/* RAM kept from anonymous memory */
#define	VS_GONE		0x80000000	/* map entry freed during its write */

struct vs_area {
	dev_t	a_dev;		/* disk; a_vpg 0: slot unused */
	struct vnode *a_vp, *a_vvp;	/* disk, virtual area */
	int	a_lowarg;	/* first block as added */
	daddr_t	a_low;		/* its first block */
	u_long	a_npg;		/* disk pages */
	u_long	a_base;		/* index of the first one among all areas */
	u_long	a_nfree;
	u_long	a_rotor;
	u_char	*a_used;	/* disk page bitmap */
	u_long	a_vpg;		/* virtual pages of this minor */
	u_long	*a_map;		/* virtual page -> disk page index + 1 */
};

/* one virtual request, done as a run of disk requests */
struct vs_req {
	struct buf r_buf;	/* first: the iodone gets its address */
	struct buf *r_bp;
	struct vs_area *r_a;
	u_long	r_pg, r_n, r_i, r_k;
	int	r_err;
	struct vs_req *r_next;
	char	r_busy;
};

static struct vs_area vs_area[VS_NAREA];
static int vs_narea, vs_cur;
static long vs_ram, vs_npg;	/* RAM part, disk pages */
static long vs_charged;		/* pages of availrmem held for reservations */
static u_long vs_nextbase;
static struct vs_req vs_req[VS_NREQ], *vs_reqfree;
static struct buf *vs_wait, *vs_waitend;
static int vs_flag = D_NEW | D_NOBRKUP;
static void vs_next();
static int vs_done();

/* index + 1 of a free disk page, or 0 */
static u_long
vs_alloc()
{
	register struct vs_area *a;
	register u_long i, n;
	int k;

	for (k = 0; k < vs_narea; k++, vs_cur++) {
		if (vs_cur >= vs_narea)
			vs_cur = 0;
		a = &vs_area[vs_cur];
		if (a->a_nfree == 0)
			continue;
		for (i = a->a_rotor, n = a->a_npg; n; n--, i++) {
			if (i >= a->a_npg)
				i = 0;
			if (!(a->a_used[i >> 3] & (1 << (i & 7)))) {
				a->a_used[i >> 3] |= 1 << (i & 7);
				a->a_nfree--;
				a->a_rotor = i + 1;
				return a->a_base + i + 1;
			}
		}
	}
	return 0;
}

static struct vs_area *
vs_disk(g)
	register u_long g;
{
	register struct vs_area *a;

	for (a = vs_area; a < &vs_area[vs_narea]; a++)
		if (g >= a->a_base && g < a->a_base + a->a_npg)
			return a;
	return 0;
}

static void
vs_release(g)
	u_long g;
{
	register struct vs_area *a = vs_disk(--g);

	if (a) {
		g -= a->a_base;
		a->a_used[g >> 3] &= ~(1 << (g & 7));
		a->a_nfree++;
	}
}

static void
vs_fail(bp, err)
	register struct buf *bp;
	int err;
{
	bp->b_flags |= B_ERROR;
	bp->b_error = err;
	bp->b_resid = bp->b_bcount;
	biodone(bp);
}

static void
vs_start(r, bp)
	register struct vs_req *r;
	register struct buf *bp;
{
	r->r_bp = bp;
	r->r_a = &vs_area[getminor(bp->b_edev)];
	r->r_pg = bp->b_blkno / VS_PGBLK;
	r->r_n = (bp->b_bcount + VS_PGSZ - 1) >> VS_PGSHIFT;
	r->r_i = 0;
	r->r_err = 0;
	r->r_busy = 1;
	vs_next(r);
}

/* the next disk run of r, or the end of its virtual request */
static void
vs_next(r)
	register struct vs_req *r;
{
	register struct buf *bp = r->r_bp, *cb = &r->r_buf;
	register struct vs_area *d;
	register u_long *map = &r->r_a->a_map[r->r_pg];
	u_long g, k, off, len;
	int s;

	while (r->r_i < r->r_n && !r->r_err) {
		g = map[r->r_i] & ~VS_GONE;
		off = r->r_i << VS_PGSHIFT;
		len = bp->b_bcount - off;
		if (g == 0) {
			bzero(bp->b_un.b_addr + off, len < VS_PGSZ ? len : VS_PGSZ);
			r->r_i++;
			continue;
		}
		d = vs_disk(g - 1);
		for (k = 1; r->r_i + k < r->r_n && (map[r->r_i + k] & ~VS_GONE) == g + k &&
		    g - 1 + k < d->a_base + d->a_npg; k++)
			;
		if (len > k << VS_PGSHIFT)
			len = k << VS_PGSHIFT;
		bzero((caddr_t)cb, sizeof *cb);
		cb->b_flags = B_BUSY | VS_CALL | (bp->b_flags & B_READ);
		cb->b_edev = d->a_dev;
		cb->b_dev = cmpdev(d->a_dev);
		cb->b_blkno = d->a_low + (g - 1 - d->a_base) * VS_PGBLK;
		cb->b_bcount = cb->b_bufsize = len;
		cb->b_un.b_addr = bp->b_un.b_addr + off;
		cb->b_proc = bp->b_proc;
		cb->b_iodone = vs_done;
		r->r_k = k;
		(*bdevsw[getmajor(d->a_dev)].d_strategy)(cb);
		return;
	}
	if (r->r_err) {
		bp->b_flags |= B_ERROR;
		bp->b_error = r->r_err;
		bp->b_resid = bp->b_bcount;
	} else
		bp->b_resid = 0;
	s = splhi();
	for (k = 0; k < r->r_n; k++)
		if (map[k] & VS_GONE) {
			vs_release(map[k] & ~VS_GONE);
			map[k] = 0;
		}
	r->r_busy = 0;
	if ((bp = vs_wait) != 0) {
		vs_wait = bp->av_forw;
		splx(s);
		biodone(r->r_bp);
		vs_start(r, bp);
		return;
	}
	r->r_next = vs_reqfree;
	vs_reqfree = r;
	bp = r->r_bp;
	splx(s);
	biodone(bp);
}

static int
vs_done(cb)
	struct buf *cb;
{
	register struct vs_req *r = (struct vs_req *)cb;

	if (cb->b_flags & B_ERROR)
		r->r_err = cb->b_error ? cb->b_error : EIO;
	else
		r->r_i += r->r_k;
	vs_next(r);
	return 0;
}

static int
vsstrategy(bp)
	register struct buf *bp;
{
	register struct vs_area *a;
	register struct vs_req *r;
	register u_long i, pg, n, *map;
	int s;

	a = &vs_area[getminor(bp->b_edev)];
	pg = bp->b_blkno / VS_PGBLK;
	n = (bp->b_bcount + VS_PGSZ - 1) >> VS_PGSHIFT;
	if (getminor(bp->b_edev) >= vs_narea || bp->b_blkno < 0 ||
	    bp->b_blkno % VS_PGBLK || pg + n > a->a_vpg) {
		vs_fail(bp, ENXIO);
		return 0;
	}
	if (!(bp->b_flags & B_READ)) {
		s = splhi();
		for (map = &a->a_map[pg], i = 0; i < n; i++) {
			/* reused before the old write ended: keep its disk page */
			map[i] &= ~VS_GONE;
			if (map[i] == 0 && (map[i] = vs_alloc()) == 0) {
				splx(s);
				vs_fail(bp, ENOSPC);
				return 0;
			}
		}
		splx(s);
	}
	if (bp->b_flags & B_PAGEIO)
		bp_mapin(bp);
	s = splhi();
	if ((r = vs_reqfree) == 0) {
		bp->av_forw = 0;
		if (vs_wait)
			vs_waitend->av_forw = bp;
		else
			vs_wait = bp;
		vs_waitend = bp;
		splx(s);
		return 0;
	}
	vs_reqfree = r->r_next;
	splx(s);
	vs_start(r, bp);
	return 0;
}

/*ARGSUSED*/
static int
vsopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	return getminor(*devp) < vs_narea && vs_area[getminor(*devp)].a_vpg ? 0 : ENXIO;
}

/*ARGSUSED*/
static int
vsclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	return 0;
}

static int
vssize(dev)
	dev_t dev;
{
	return getminor(dev) < vs_narea ?
	    (int)(vs_area[getminor(dev)].a_vpg * VS_PGBLK) : -1;
}

/*ARGSUSED*/
static int
vsprint(dev, s)
	dev_t dev;
	char *s;
{
	printf("vswap: %s\n", s);
	return 0;
}

/* the block device row, once */
static int
vs_install()
{
	register struct bdevsw *b = &bdevsw[VS_ROW];
	int i;

	if (b->d_strategy == vsstrategy)
		return 0;
	if (VS_ROW >= bdevcnt || b->d_open != bdevsw[0].d_open ||
	    b->d_strategy != bdevsw[0].d_strategy || b->d_size != bdevsw[0].d_size) {
		printf("vswap: block device row %d is taken\n", VS_ROW);
		return EBUSY;
	}
	for (i = 0; i < VS_NREQ; i++) {
		vs_req[i].r_next = vs_reqfree;
		vs_reqfree = &vs_req[i];
	}
	b->d_open = vsopen;
	b->d_close = vsclose;
	b->d_print = vsprint;
	b->d_size = vssize;
	b->d_flag = &vs_flag;
	b->d_strategy = vsstrategy;
	return 0;
}

/* reservations beyond the disks, in pages */
static long
vs_need(resv)
	long resv;
{
	return resv > vs_npg ? resv - vs_npg : 0;
}

/* availrmem holds what the reservations need now */
static void
vs_recharge()
{
	long n;
	int s = splhi();

	n = vs_need((long)anoninfo.ani_resv);
	availrmem += vs_charged - n;
	vs_charged = n;
	splx(s);
}

static long
vs_keep()
{
	return physmem / 8 > VS_KEEP ? physmem / 8 : VS_KEEP;
}

/* a disk area becomes the backing store of a new virtual area */
int
swapadd(vp, lowblk, nblks, name)
	struct vnode *vp;
	int lowblk, nblks;
	char *name;
{
	register struct vs_area *a, *b;
	struct vnode *bvp = vp, *vvp;
	long sz, lo, hi, npg, ram = 0;
	int err, first;

	if (vp->v_type != VBLK || getmajor(vp->v_rdev) == VS_ROW || vs_install())
		return __vm_swapadd(vp, lowblk, nblks, name);
	for (a = vs_area; a < &vs_area[vs_narea] && a->a_vpg; a++)
		;
	if (a == &vs_area[VS_NAREA])
		return __vm_swapadd(vp, lowblk, nblks, name);
	first = vs_ram == 0;
	if (lowblk < 0)
		return EINVAL;
	lo = (lowblk + VS_PGBLK - 1) & ~(VS_PGBLK - 1);
	/* the same blocks twice would give two processes one block */
	for (b = vs_area; b < &vs_area[vs_narea]; b++)
		if (b->a_vpg && b->a_dev == vp->v_rdev && (b->a_lowarg == lowblk ||
		    (lowblk < b->a_low + (long)b->a_npg * VS_PGBLK &&
		    b->a_low < lowblk + nblks)))
			return EEXIST;
	if ((err = VOP_OPEN(&bvp, FREAD | FWRITE, u.u_cred)) != 0)
		return err;
	sz = (*bdevsw[getmajor(bvp->v_rdev)].d_size)(bvp->v_rdev);
	hi = lowblk + nblks;
	if (sz > 0 && hi > sz)
		hi = sz;
	hi &= ~(VS_PGBLK - 1);
	npg = hi > lo ? (hi - lo) / VS_PGBLK : 0;
	if (first && (ram = availrmem - tune.t_minarmem) < 0)
		ram = 0;
	/* a unit smaller than a page: swap is RAM alone */
	if (npg + ram <= 0) {
		VOP_CLOSE(bvp, FREAD | FWRITE, 1, 0, u.u_cred);
		return EINVAL;
	}
	a->a_dev = bvp->v_rdev;
	a->a_lowarg = lowblk;
	a->a_low = lo;
	a->a_npg = a->a_nfree = npg;
	a->a_base = vs_nextbase;
	a->a_rotor = 0;
	a->a_used = (u_char *)kmem_zalloc((npg + 8) >> 3, KM_SLEEP);
	a->a_map = (u_long *)kmem_zalloc((npg + ram) * sizeof (u_long), KM_SLEEP);
	a->a_vpg = npg + ram;
	if (a == &vs_area[vs_narea])
		vs_narea++;
	vvp = makespecvp(makedevice(VS_ROW, a - vs_area), VBLK);
	err = __vm_swapadd(vvp, 0, (int)(a->a_vpg * VS_PGBLK), name);
	if (err) {
		kmem_free((caddr_t)a->a_used, (npg + 8) >> 3);
		kmem_free((caddr_t)a->a_map, a->a_vpg * sizeof (u_long));
		a->a_vpg = a->a_npg = a->a_nfree = 0;
		if (a == &vs_area[vs_narea - 1])
			vs_narea--;
		VN_RELE(vvp);
		VOP_CLOSE(bvp, FREAD | FWRITE, 1, 0, u.u_cred);
		return err;
	}
	VN_HOLD(bvp);
	a->a_vp = bvp;
	a->a_vvp = vvp;
	vs_nextbase += npg;
	/* RAM is in availsmem already */
	availsmem -= ram;
	vs_ram += ram;
	vs_npg += npg;
	vs_recharge();
	return 0;
}

/* swap -d of a disk: its virtual area goes, the area with the RAM part stays */
int
swapdel(vp, lowblk)
	struct vnode *vp;
	u_int lowblk;
{
	register struct vs_area *a;
	int err;

	for (a = vs_area; a < &vs_area[vs_narea]; a++)
		if (a->a_vpg && vp->v_type == VBLK && a->a_dev == vp->v_rdev &&
		    a->a_lowarg == lowblk)
			break;
	if (a == &vs_area[vs_narea])
		return __vm_swapdel(vp, lowblk);
	if (a->a_vpg > a->a_npg)
		return EBUSY;
	if (vs_ram && vs_need((long)anoninfo.ani_resv + (long)a->a_npg) - vs_charged >
	    availrmem - vs_keep())
		return ENOMEM;
	if ((err = __vm_swapdel(a->a_vvp, 0)) != 0)
		return err;
	vs_npg -= a->a_npg;
	vs_recharge();
	kmem_free((caddr_t)a->a_used, (a->a_npg + 8) >> 3);
	kmem_free((caddr_t)a->a_map, a->a_vpg * sizeof (u_long));
	a->a_vpg = a->a_npg = a->a_nfree = 0;
	VN_RELE(a->a_vvp);
	VOP_CLOSE(a->a_vp, FREAD | FWRITE, 1, 0, u.u_cred);
	VN_RELE(a->a_vp);
	return 0;
}

/* a write in flight on virtual page pg of a */
static int
vs_writing(a, pg)
	struct vs_area *a;
	u_long pg;
{
	register struct vs_req *r;

	for (r = vs_req; r < &vs_req[VS_NREQ]; r++)
		if (r->r_busy && r->r_a == a && !(r->r_bp->b_flags & B_READ) &&
		    pg >= r->r_pg && pg < r->r_pg + r->r_n)
			return 1;
	return 0;
}

/* a freed slot gives its disk page back */
int
swap_free(ap)
	struct anon *ap;
{
	struct vnode *vp = 0;
	u_int off;
	register struct vs_area *a;
	register u_long *m;
	int s;

	if (vs_narea) {
		swap_xlate(ap, &vp, &off);
		if (vp && vp->v_type == VBLK && getmajor(vp->v_rdev) == VS_ROW &&
		    getminor(vp->v_rdev) < vs_narea) {
			a = &vs_area[getminor(vp->v_rdev)];
			m = &a->a_map[off >> VS_PGSHIFT];
			s = splhi();
			if (off >> VS_PGSHIFT < a->a_vpg && *m) {
				if (vs_writing(a, (u_long)(off >> VS_PGSHIFT)))
					*m |= VS_GONE;
				else {
					vs_release(*m);
					*m = 0;
				}
			}
			splx(s);
		}
	}
	return __vm_swap_free(ap);
}

/*
 * Beyond the disks a reservation takes RAM from availrmem, leaving a
 * reserve for the pager.
 */
int
anon_resv(size)
	u_int size;
{
	long d;
	int s, ok;

	if (!vs_ram)
		return __vm_anon_resv(size);
	s = splhi();
	d = vs_need((long)(anoninfo.ani_resv + ((size + VS_PGSZ - 1) >> VS_PGSHIFT))) -
	    vs_charged;
	if (d > 0 && d > availrmem - vs_keep())
		ok = 0;
	else if ((ok = __vm_anon_resv(size)) != 0 && d > 0) {
		availrmem -= d;
		vs_charged += d;
	}
	splx(s);
	return ok;
}

void
anon_unresv(size)
	u_int size;
{
	__vm_anon_unresv(size);
	if (vs_ram)
		vs_recharge();
}
