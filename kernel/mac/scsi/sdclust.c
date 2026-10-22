/*
 * Disk request shaping for the 53C96: block requests at adjacent blocks
 * go out as one command, sequential reads fetch up to 64 KB ahead, and
 * raw I/O moves 64 KB per command.  Each unit gets a 64 KB cache buffer
 * and a 64 KB raw bounce buffer on first use.
 *
 * The cache holds what the disk holds: reads fill it, merged writes go
 * through it, other writes drop what they overlap.  Writes that bypass
 * this code (stock paths while switched off) bump gen, which empties it.
 */
#ifdef NCR_HOST
#include	"sdchost.h"
#else
#include	"sys/types.h"
#include	"sys/param.h"
#include	"sys/buf.h"
#include	"sys/uio.h"
#include	"sys/errno.h"
#include	"sys/kmem.h"
#include	"rico.h"
#include	"sd.h"
#include	"macspl.h"

extern bool	sdvalid();
extern int	uiophysio(), sleep(), wakeup(), printf();
extern uint	sdblkno(), sddevsize();
extern void	iodone(), sdqueue(), bcopy(), dlm_cachepush();
extern char	*sdhardwarename();
extern paddr_t	vtop();
extern int	(*__volatile__ sd_strathook)(), (*__volatile__ sd_rawhook)();
#endif

#define	BSIZE		512
#define	CBYTES		0x10000
#define	CBLKS		(CBYTES / BSIZE)
#define	MAXRUN		32
#define	PGSTEP		0x800		/* walks user buffers in steps no larger than a page */
#define	KVA		2		/* sdcom.reading: addr is a kernel VA, no cache push */
#ifndef sdunit
#define	sdunit(dev)	((dev) & 07)
#define	sdcard(dev)	((dev) >> 3 & 01)
#define	byte3(i)	((uint)(i) >> 24 & 0xFF)
#define	byte2(i)	((uint)(i) >> 16 & 0xFF)
#define	byte1(i)	((uint)(i) >> 8 & 0xFF)
#define	byte0(i)	((uint)(i) & 0xFF)
#endif

/* command states, as the stock disk driver */
#define	FIRST		0
#define	SENSE		1
#define	RETRY		2

struct sreq {
	struct sdcom	com;		/* first: the completion gets it */
	int		state;
	bool		fin, err;
	uchar		cdb0;
	uint		bn, nb;
	caddr_t		addr;
	char		reading;
	uchar		sense[16];
	void		(*done)();
	struct cu	*u;
};

struct cu {
	struct buf	*bh, *bt;	/* block queue; the running request(s) at the head */
	struct sreq	rq;
	int		nrun;		/* queued bufs the running command serves */
	bool		busy, viac;	/* command running; through the cache buffer */
	uint		rbn, rnb;	/* running command */
	ulong		rgen;
	int		solo;		/* head bufs to issue alone (after a merged error) */
	caddr_t		cb;		/* cache buffer */
	uint		cbn, cnb;	/* blocks it holds */
	ulong		cgen, gen;
	uint		seq;		/* block after the last block read */
	caddr_t		bb;		/* raw bounce */
	bool		rawbusy;
	struct sreq	raw;
};


/* tunables */
int	sd_clust = 0;			/* merge and read ahead (0: stock queue) */
int	sd_ra = CBLKS;			/* read-ahead blocks, at most CBLKS */
int	sd_bigraw = 0;			/* raw: 64 KB commands (0: stock 2 KB) */

/* counters */
long	sd_nreq = 0, sd_ncmd = 0, sd_nmerge = 0, sd_nra = 0, sd_nhit = 0;
long	sd_nsplit = 0, sd_nraw = 0, sd_ndirect = 0, sd_nbounce = 0;

static struct cu	cus[SDCARDS * SDUNITS];
static void	sdc_start(), sdc_done(), sq_issue();

static struct cu *
unit(dev)
dev_t dev;
{
	return &cus[sdcard(dev) * SDUNITS + sdunit(dev)];
}

static void
sq_intr(cp)
struct sdcom *cp;
{
	struct sreq *rq = (struct sreq *)cp;

	if (!cp->okay)
		rq->err = TRUE;
	else
		switch (rq->state) {
		case FIRST:
			if (cp->status != 0) {
				cp->addr = (caddr_t)rq->sense;
				cp->reading = TRUE | KVA;
				cp->cdb[0] = 0x03;
				cp->cdb[1] = cp->cdb[2] = cp->cdb[3] = cp->cdb[5] = 0;
				cp->cdb[4] = sizeof rq->sense;
				cp->nbyte = sizeof rq->sense;
				rq->state = SENSE;
				sdqueue(cp);
				return;
			}
			break;
		case SENSE:
			if (cp->status == 0) {
				printf("%s: disk c%dd0: blk%d 0x%x 0x%x\n",
				    sdhardwarename(cp->card), cp->card * 8 + cp->unit,
				    rq->bn, rq->sense[2], rq->sense[12]);
				sq_issue(rq, RETRY);
				return;
			}
			rq->err = TRUE;
			break;
		case RETRY:
			if (cp->status != 0)
				rq->err = TRUE;
		}
	rq->fin = TRUE;
	(*rq->done)(rq);
}

static void
sq_issue(rq, st)
struct sreq *rq;
int st;
{
	struct sdcom *cp = &rq->com;

	cp->addr = rq->addr;
	cp->reading = rq->reading;
	cp->cdb[0] = rq->cdb0;
	cp->cdb[1] = 0;
	cp->cdb[2] = byte3(rq->bn);
	cp->cdb[3] = byte2(rq->bn);
	cp->cdb[4] = byte1(rq->bn);
	cp->cdb[5] = byte0(rq->bn);
	cp->cdb[6] = 0;
	cp->cdb[7] = byte1(rq->nb);
	cp->cdb[8] = byte0(rq->nb);
	cp->cdb[9] = 0;
	cp->nbyte = rq->nb * BSIZE;
	cp->intr = sq_intr;
	rq->state = st;
	sdqueue(cp);
}

static void
sq_start(rq, dev, rd, bn, nb, addr, kva)
struct sreq *rq;
dev_t dev;
int rd, kva;
uint bn, nb;
caddr_t addr;
{
	sd_ncmd++;
	rq->com.card = sdcard(dev);
	rq->com.unit = sdunit(dev);
	rq->fin = rq->err = FALSE;
	rq->cdb0 = rd ? 0x28 : 0x2A;
	rq->bn = bn;
	rq->nb = nb;
	rq->addr = addr;
	rq->reading = (rd ? TRUE : FALSE) | (kva ? KVA : 0);
	sq_issue(rq, FIRST);
}

/* Drop cached blocks that [bn, bn+nb) overlaps. */
static void
drop(u, bn, nb)
struct cu *u;
uint bn, nb;
{
	if (u->cnb && bn < u->cbn + u->cnb && u->cbn < bn + nb)
		u->cnb = 0;
}

static bool
plain(bp)
struct buf *bp;
{
	return bp->b_proc == 0 && bp->b_bcount > 0 && bp->b_bcount <= CBYTES
	    && bp->b_bcount % BSIZE == 0;
}

/* Copy between a kernel buffer and the cache; low addresses are physical. */
static void
bcp(bp, c, n, tobuf)
struct buf *bp;
caddr_t c;
uint n;
int tobuf;
{
	caddr_t a = bp->b_un.b_addr;

	if ((ulong)a < 0x40000000)
		dlm_cachepush(a, n);
	if (tobuf)
		bcopy(c, a, n);
	else
		bcopy(a, c, n);
}

/*
 * Block strategy.  Requests queue per unit; the head runs, merged with
 * the queued requests that follow it on disk, or comes from the cache.
 */
static int
sdc_strat(bp, stock)
struct buf *bp;
int (*stock)();
{
	struct cu *u = unit(bp->b_edev);
	int x;

	if (!sd_clust) {
		x = splscsi();
		u->gen++;
		splrestore(x);
		return (*stock)(bp);
	}
	if (!u->cb)
		u->cb = (caddr_t)kmem_alloc(CBYTES, KM_NOSLEEP);
	if (!sdvalid(bp))
		return 0;
	sd_nreq++;
	bp->av_forw = 0;
	bp->b_resid = 0;
	x = splscsi();
	if (u->bh)
		u->bt->av_forw = bp;
	else
		u->bh = bp;
	u->bt = bp;
	if (!u->busy)
		sdc_start(u);
	splrestore(x);
	return 0;
}

static void
sdc_start(u)
struct cu *u;
{
	struct buf *bp, *b;
	uint bn, nb, lim, left;
	int rd;

	while ((bp = u->bh) && !u->busy) {
		bn = sdblkno(bp);
		nb = bp->b_bcount / BSIZE;
		rd = bp->b_flags & B_READ;
		if (rd && !u->solo && plain(bp) && u->cnb && u->cgen == u->gen
		&& bn >= u->cbn && bn + nb <= u->cbn + u->cnb) {
			bcp(bp, u->cb + (bn - u->cbn) * BSIZE, bp->b_bcount, TRUE);
			sd_nhit++;
			u->seq = bn + nb;
			u->bh = bp->av_forw;
			iodone(bp);
			continue;
		}
		u->busy = TRUE;
		u->nrun = 1;
		u->rbn = bn;
		u->rnb = nb;
		u->rgen = u->gen;
		u->viac = FALSE;
		if (u->solo)
			u->solo--;
		else if (plain(bp) && u->cb) {
			for (b = bp->av_forw; b && u->nrun < MAXRUN; b = b->av_forw) {
				if ((b->b_flags & B_READ) != rd || !plain(b)
				|| sdblkno(b) != bn + u->rnb
				|| u->rnb + b->b_bcount / BSIZE > CBLKS)
					break;
				u->nrun++;
				u->rnb += b->b_bcount / BSIZE;
			}
			sd_nmerge += u->nrun - 1;
			lim = sd_ra <= 0 ? 0 : sd_ra < CBLKS ? sd_ra : CBLKS;
			left = sddevsize(bp->b_edev) - bp->b_blkno;
			if (lim > left)
				lim = left;
			if (rd && bn == u->seq && lim > u->rnb) {
				u->rnb = lim;
				sd_nra++;
			}
			u->viac = u->nrun > 1 || u->rnb > nb;
		}
		if (!rd)
			drop(u, u->rbn, u->rnb);
		u->rq.done = sdc_done;
		u->rq.u = u;
		if (u->viac) {
			u->cnb = 0;
			if (!rd)
				for (b = bp, nb = 0; nb < u->rnb; b = b->av_forw) {
					bcp(b, u->cb + nb * BSIZE, b->b_bcount, FALSE);
					nb += b->b_bcount / BSIZE;
				}
			sq_start(&u->rq, bp->b_edev, rd, u->rbn, u->rnb, u->cb, TRUE);
		} else
			sq_start(&u->rq, bp->b_edev, rd, bn, nb,
			    (caddr_t)vtop(bp->b_un.b_addr, bp->b_proc), FALSE);
	}
}

/* Command done, at interrupt level. */
static void
sdc_done(rq)
struct sreq *rq;
{
	struct cu *u = rq->u;
	struct buf *bp;
	uint off;
	int i, rd;

	bp = u->bh;
	rd = bp->b_flags & B_READ;
	if (rq->err && u->viac) {
		/* retry each request on its own */
		sd_nsplit++;
		u->solo = u->nrun;
		u->busy = FALSE;
		sdc_start(u);
		return;
	}
	if (u->viac && !rq->err) {
		u->cbn = u->rbn;
		u->cnb = u->rnb;
		u->cgen = u->rgen;
	}
	for (i = 0, off = 0; i < u->nrun; i++) {
		bp = u->bh;
		u->bh = bp->av_forw;
		if (rq->err)
			bp->b_flags |= B_ERROR;
		else if (u->viac && rd)
			bcp(bp, u->cb + off, bp->b_bcount, TRUE);
		off += bp->b_bcount;
		if (rd)
			u->seq = sdblkno(bp) + bp->b_bcount / BSIZE;
		iodone(bp);
	}
	u->busy = FALSE;
	sdc_start(u);
}

static void
raw_done(rq)
struct sreq *rq;
{
	wakeup((caddr_t)rq);
}

/* Physical address of [a, a+n) in p if contiguous, even and directly addressable, else 0. */
static paddr_t
contig(a, n, p)
caddr_t a;
uint n;
struct proc *p;
{
	paddr_t pa, q;
	uint o;

	pa = vtop(a, p);
	if (pa == 0 || (pa & 1) || pa + n > 0x40000000)
		return 0;
	for (o = PGSTEP - ((ulong)a & (PGSTEP - 1)); o < n; o += PGSTEP) {
		q = vtop(a + o, p);
		if (q != pa + o)
			return 0;
	}
	return pa;
}

/* Copy between locked user pages and the bounce buffer, page by page. */
static void
ucp(a, c, n, p, tou)
caddr_t a, c;
uint n;
struct proc *p;
int tou;
{
	uint o, k;
	caddr_t pa;

	for (o = 0; o < n; o += k) {
		k = PGSTEP - ((ulong)(a + o) & (PGSTEP - 1));
		if (k > n - o)
			k = n - o;
		pa = (caddr_t)vtop(a + o, p);
		dlm_cachepush(pa, k);
		if (tou)
			bcopy(c + o, pa, k);
		else
			bcopy(pa, c + o, k);
	}
}

/*
 * Raw strategy under uiophysio (user pages locked): 64 KB commands, into
 * the user's pages when they are physically contiguous, else through the
 * bounce buffer.
 */
static int
sdc_rawstrat(bp)
struct buf *bp;
{
	struct cu *u = unit(bp->b_edev);
	struct sreq *rq = &u->raw;
	caddr_t a, addr;
	uint bn, n, left, off;
	int rd, x, kva;

	if (!sdvalid(bp))
		return 0;
	rd = bp->b_flags & B_READ;
	bn = sdblkno(bp);
	bp->b_resid = 0;
	x = splscsi();
	while (u->rawbusy)
		sleep((caddr_t)&u->rawbusy, PRIBIO);
	u->rawbusy = TRUE;
	splrestore(x);
	for (off = 0, left = bp->b_bcount; left > 0; off += n, left -= n) {
		n = left < CBYTES ? left : CBYTES;
		a = bp->b_un.b_addr + off;
		kva = FALSE;
		if (bp->b_proc == 0) {
			addr = a;
			kva = TRUE;
		} else if ((addr = (caddr_t)contig(a, n, bp->b_proc)) != 0)
			sd_ndirect++;
		else {
			if (!u->bb && !(u->bb = (caddr_t)kmem_alloc(CBYTES, KM_SLEEP)))
				break;
			if (!rd)
				ucp(a, u->bb, n, bp->b_proc, FALSE);
			addr = u->bb;
			kva = TRUE;
			sd_nbounce++;
		}
		sd_nraw++;
		rq->done = raw_done;
		rq->u = u;
		x = splscsi();
		if (!rd)
			drop(u, bn, n / BSIZE);
		sq_start(rq, bp->b_edev, rd, bn, n / BSIZE, addr, kva);
		while (!rq->fin)
			sleep((caddr_t)rq, PRIBIO);
		if (!rd)
			drop(u, bn, n / BSIZE);
		splrestore(x);
		if (rq->err)
			break;
		if (rd && addr == u->bb)
			ucp(a, u->bb, n, bp->b_proc, TRUE);
		bn += n / BSIZE;
	}
	if (left) {
		bp->b_flags |= B_ERROR;
		bp->b_error = EIO;
		bp->b_resid = left;
	}
	u->rawbusy = FALSE;
	wakeup((caddr_t)&u->rawbusy);
	iodone(bp);
	return 0;
}

static int
sdc_raw(dev, uio, rw, stock, cr)
dev_t dev;
struct uio *uio;
int rw;
int (*stock)();
struct cred *cr;
{
	struct cu *u = unit(dev);
	long end;
	int x, r;

	if (!sd_bigraw || uio->uio_offset % BSIZE || uio->uio_resid % BSIZE) {
		if (rw == B_READ)
			return (*stock)(dev, uio, cr);
		x = splscsi();
		u->gen++;
		splrestore(x);
		r = (*stock)(dev, uio, cr);
		x = splscsi();
		u->gen++;
		splrestore(x);
		return r;
	}
	end = sddevsize(dev) * BSIZE;
	if (uio->uio_offset == end)
		return 0;
	if (uio->uio_offset + uio->uio_resid > end)
		uio->uio_resid = end - uio->uio_offset;
	return uiophysio(sdc_rawstrat, (struct buf *)0, dev, rw, uio);
}

void
sdc_attach()
{
	sd_strathook = sdc_strat;
	sd_rawhook = sdc_raw;
}
