/*
 * dsseg.c -- mappings of a session's frame buffer.
 *
 * A device segment: VRAM while the session is in front (page frames
 * from ds_fbmmap()), its shadow pages while hidden.  The segment is the
 * kernel's segdev with four operations wrapped:
 *   fault   refuses software locking; loads shadow pages as the managed
 *           pages they are; retries when a switch raced the load; makes
 *           VRAM pages write-through (FBC_WT);
 *   dup     tracks the child's copy;
 *   unmap   whole segment only;
 *   free    stops tracking.
 * A switch unloads every tracked translation of the two sessions; the
 * next access faults onto the new backing.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/errno.h"
#include "sys/poll.h"
#include "sys/mman.h"
#include "sys/cred.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "vm/hat.h"
#include "vm/seg_dev.h"
#include "vm/page.h"
#include "ds.h"

extern int segdev_create(), as_map(), as_unmap();
extern page_t *page_numtookpp();
extern void map_addr(), flushmmu();
extern int ds_fbmmap();
extern struct dssess *ds_fbsess();

/* 040 root table of an address space; 0 once hat_free ran */
#define AS_ROOT(as)	(*(unsigned long **)((char *)(as) + 20))
#if defined(DS_ATARI) && !defined(ATA060)
/*
 * 030: the root table lives in the address space; hat_free marks its
 * descriptors invalid and hat_unload checks them, so unloading a freed
 * space is a no-op.
 */
#define AS_FREED(as)	0
#else
#define AS_FREED(as)	(AS_ROOT(as) == 0)
#endif

struct dsseg {
	struct seg	*t_seg;		/* 0 free, DS_RESV claimed */
	struct dssess	*t_sess;
	int		t_cache;
};
#define DS_RESV		((struct seg *)1)

struct dscrargs {
	struct segdev_crargs c_dev;	/* first: segdev_create reads only this */
	struct dssess	*c_sess;
};

static struct dsseg ds_seg[DS_NSEG];
static struct seg_ops ds_segops, *ds_devops;
static int ds_sdup(), ds_sunmap();
static void ds_sfree();
static faultcode_t ds_sfault();

static struct dsseg *
ds_track(seg)
struct seg *seg;
{
	register int i;

	for (i = 0; i < DS_NSEG; i++)
		if (ds_seg[i].t_seg == seg)
			return &ds_seg[i];
	return 0;
}

static struct dsseg *
ds_claim()
{
	register int i, x;

	x = DS_SPL(DS_HI);
	for (i = 0; i < DS_NSEG; i++)
		if (ds_seg[i].t_seg == 0) {
			ds_seg[i].t_seg = DS_RESV;
			DS_SPLX(x);
			return &ds_seg[i];
		}
	DS_SPLX(x);
	return 0;
}

int
ds_segcount(s)
struct dssess *s;
{
	register int i, n = 0;

	for (i = 0; i < DS_NSEG; i++)
		if (ds_seg[i].t_seg && ds_seg[i].t_seg != DS_RESV && ds_seg[i].t_sess == s)
			n++;
	return n;
}

/* drop every user translation of s; safe context only */
void
ds_unloadsess(s)
struct dssess *s;
{
	register struct seg *seg;
	register int i;

	for (i = 0; i < DS_NSEG; i++) {
		seg = ds_seg[i].t_seg;
		if (seg == 0 || seg == DS_RESV || ds_seg[i].t_sess != s ||
		    AS_FREED(seg->s_as))
			continue;
		hat_unload(seg, seg->s_base, seg->s_size, HAT_NOFLAGS);
	}
}

static int
ds_segcreate(seg, a)
struct seg *seg;
struct dscrargs *a;
{
	register struct dsseg *t;
	register int err;

	if ((t = ds_claim()) == 0)
		return ENOMEM;
	if ((err = segdev_create(seg, (caddr_t)&a->c_dev)) != 0) {
		t->t_seg = 0;
		return err;
	}
	if (ds_devops == 0) {
		ds_devops = seg->s_ops;
		ds_segops = *ds_devops;
		ds_segops.dup = ds_sdup;
		ds_segops.unmap = ds_sunmap;
		ds_segops.free = ds_sfree;
		ds_segops.fault = ds_sfault;
	}
	seg->s_ops = &ds_segops;
	t->t_sess = a->c_sess;
	t->t_cache = a->c_sess->s_cache;
	t->t_seg = seg;
	return 0;
}

/* d_segmap: shared maps of [off, off + len) of the session's region */
int
ds_segmap(dev, off, as, addrp, len, prot, maxprot, flags, cr)
dev_t dev;
off_t off;
struct as *as;
addr_t *addrp;
u_int len, prot, maxprot, flags;
struct cred *cr;
{
	struct dscrargs a;
	register struct dssess *s = ds_fbsess(dev);
	register u_int rlen = (len + DS_PGOFF) & ~DS_PGOFF;

	if (s == 0 || s->s_dead)
		return ENODEV;
	if (!ds_owns(s))
		return EACCES;
	if ((flags & MAP_TYPE) != MAP_SHARED)
		return EINVAL;
	if (off < 0 || (off & DS_PGOFF) || rlen == 0 || rlen < len ||
	    rlen > s->s_size || off > s->s_size - rlen)
		return ENXIO;
	if (flags & MAP_FIXED)
		(void)as_unmap(as, *addrp, len);
	else {
		map_addr(addrp, len, off, 0);
		if (*addrp == 0)
			return ENOMEM;
	}
	a.c_dev.mapfunc = ds_fbmmap;
	a.c_dev.offset = off;
	a.c_dev.dev = dev;
	a.c_dev.prot = prot;
	a.c_dev.maxprot = maxprot;
	a.c_sess = s;
	return as_map(as, *addrp, len, ds_segcreate, (caddr_t)&a);
}

static int
ds_sdup(seg, nseg)
struct seg *seg, *nseg;
{
	register struct dsseg *t = ds_track(seg), *n;
	register int err;

	if (t == 0 || (n = ds_claim()) == 0)
		return ENOMEM;
	if ((err = (*ds_devops->dup)(seg, nseg)) != 0) {
		n->t_seg = 0;
		return err;
	}
	nseg->s_ops = &ds_segops;
	n->t_sess = t->t_sess;
	n->t_cache = t->t_cache;
	n->t_seg = nseg;
	return 0;
}

static int
ds_sunmap(seg, addr, len)
struct seg *seg;
addr_t addr;
u_int len;
{
	if (addr != seg->s_base || len != seg->s_size)
		return EINVAL;
	return (*ds_devops->unmap)(seg, addr, len);
}

static void
ds_sfree(seg)
struct seg *seg;
{
	register struct dsseg *t = ds_track(seg);
	register struct dssess *s = 0;

	if (t) {
		s = t->t_sess;
		t->t_seg = 0;
	}
	(*ds_devops->free)(seg);
	if (s)
		ds_sessgc(s);
}

/* the leaf entries of [addr, addr + len) that map VRAM: write-through */
static void
ds_setwt(seg, addr, len)
struct seg *seg;
addr_t addr;
u_int len;
{
	register unsigned long va, e, a, b, lo, hi;
	register VOL unsigned long *leaf;
	register unsigned long *root = AS_ROOT(seg->s_as);
	int done = 0;

	if (root == 0)
		return;
	lo = ds_disp.d_page >> DS_PGSHIFT;
	hi = lo + (ds_disp.d_vsize >> DS_PGSHIFT);
	e = (unsigned long)addr + len;
	for (va = (unsigned long)addr & ~DS_PGOFF; va < e; va += DS_PGSIZE) {
		a = root[(va >> 25) & 0x7F];
		if ((a & 2) == 0)
			continue;
		b = ((unsigned long *)(a & 0xFFFFFE00))[(va >> 18) & 0x7F];
		if ((b & 2) == 0)
			continue;
		leaf = (unsigned long *)(b & 0xFFFFFF00) + ((va >> 12) & 0x3F);
		a = *leaf;
		if ((a & 1) == 0 || (a >> 12) < lo || (a >> 12) >= hi)
			continue;
		*leaf = a & ~0x60;
		done = 1;
	}
	if (done)
		flushmmu(addr, 1);
}

/*
 * A hidden session's pages: its shadow, kernel memory with page
 * structures, loaded with them so that unloading finds the mappings.
 */
static faultcode_t
ds_shfault(seg, s, addr, len, rw)
struct seg *seg;
struct dssess *s;
addr_t addr;
u_int len;
enum seg_rw rw;
{
	register struct segdev_data *sd = (struct segdev_data *)seg->s_data;
	register unsigned long va, off;
	register page_t *pp;
	u_int pv[4];

	for (va = (unsigned long)addr & ~DS_PGOFF; va < (unsigned long)addr + len;
	    va += DS_PGSIZE) {
		off = sd->offset + (va - (unsigned long)seg->s_base);
		pv[0] = 0;
		(void)(*ds_devops->getprot)(seg, (addr_t)va, 0, pv);
		if ((pv[0] & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0 ||
		    (rw == S_WRITE && !(pv[0] & PROT_WRITE)))
			return FC_PROT;
		if (off >= s->s_size ||
		    (pp = page_numtookpp(s->s_pfn[off >> DS_PGSHIFT])) == 0)
			return FC_MAKE_ERR(EFAULT);
		hat_memload(seg, (addr_t)va, pp, pv[0], HAT_NOFLAGS);
	}
	return 0;
}

static faultcode_t
ds_sfault(seg, addr, len, type, rw)
struct seg *seg;
addr_t addr;
u_int len;
enum fault_type type;
enum seg_rw rw;
{
	register struct dsseg *t = ds_track(seg);
	register faultcode_t fc;
	register unsigned long g;
	register int tries;

	if (t == 0 || t->t_sess->s_dead)
		return FC_MAKE_ERR(ENXIO);
	if (type == F_SOFTLOCK || type == F_SOFTUNLOCK)
		return FC_MAKE_ERR(EFAULT);
	for (tries = 0; ; tries++) {
		g = ds_gen;
		if (t->t_sess == ds_front)
			fc = (*ds_devops->fault)(seg, addr, len, type, rw);
		else
			fc = ds_shfault(seg, t->t_sess, addr, len, rw);
		if (fc != 0)
			return fc;
		if (g == ds_gen)
			break;
		hat_unload(seg, (addr_t)((unsigned long)addr & ~DS_PGOFF),
		    (len + ((unsigned long)addr & DS_PGOFF) + DS_PGOFF) & ~DS_PGOFF,
		    HAT_NOFLAGS);
		if (tries >= 8 || t->t_sess->s_dead)
			return FC_MAKE_ERR(EAGAIN);
	}
	if (t->t_cache == FBC_WT && t->t_sess == ds_front)
		ds_setwt(seg, addr, len);
	return 0;
}
