/*
 * bigram -- Zorro III RAM that Unix leaves alone (memory boards above
 * 1 GB, such as a BigRamPlus) as a block device, for swap:
 *
 *	modadmin -l bigram
 *	mknod /dev/dsk/bigram b <major> 0	(the major is printed at load)
 *	swap -a /dev/dsk/bigram 0 <blocks>	(512-byte blocks; swap -l lists KB)
 *
 * The kernel uses RAM only in the low gigabyte, so a Zorro III memory
 * board is free for this.  At load the module maps the largest such
 * board and claims a free row of bdevsw, as the floppy service hooks
 * its row; unload gives the row back unless the device is open.
 * Requests are copies to and from the board through its uncached
 * supervisor mapping.
 *
 * K&R C.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sys/buf.h"
#include "sys/conf.h"
#include "sys/open.h"
#include "sys/moddefs.h"
#include "sys/sysmacros.h"
#include "sys/systm.h"
#include "sys/inline.h"
#include "amilib.h"

extern int nodev(), nulldev(), printf(), plat_zorro();
extern char *plat_iomap();
extern void bcopy(), biodone(), bp_mapin(), bp_mapout();

#define	BR_FIRSTROW	21		/* rows 0-15 and 21-31 are free in the AMIX table */
#define	ERTF_MEMLIST	0x20
#define	BR_MINSIZE	0x100000UL

static char *br_base;			/* the board, mapped */
static unsigned long br_pa, br_size;
static int br_row = -1, br_open;
static int br_flag = D_NEW | D_NOBRKUP;
static struct bdevsw br_saved;

/*ARGSUSED*/
static int
bropen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	if (getminor(*devp) != 0)
		return ENXIO;
	br_open = 1;
	return 0;
}

/*ARGSUSED*/
static int
brclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	br_open = 0;
	return 0;
}

static int
brsize(dev)
	dev_t dev;
{
	return getminor(dev) == 0 ? (int)(br_size >> 9) : -1;
}

/*ARGSUSED*/
static int
brprint(dev, s)
	dev_t dev;
	char *s;
{
	printf("bigram: %s\n", s);
	return 0;
}

static int
brstrategy(bp)
	register struct buf *bp;
{
	unsigned long off = (unsigned long)bp->b_blkno << 9, n = bp->b_bcount;
	int mapped = 0;

	if (getminor(bp->b_edev) != 0 || off >= br_size || n > br_size - off) {
		bp->b_flags |= B_ERROR;
		bp->b_error = ENXIO;
		bp->b_resid = n;
		biodone(bp);
		return 0;
	}
	if (bp->b_flags & B_PAGEIO) {
		bp_mapin(bp);
		mapped = 1;
	}
	if (bp->b_flags & B_READ)
		bcopy(br_base + off, bp->b_un.b_addr, n);
	else
		bcopy(bp->b_un.b_addr, br_base + off, n);
	if (mapped)
		bp_mapout(bp);
	bp->b_resid = 0;
	biodone(bp);
	return 0;
}

static int
bigram_load()
{
	struct amx_zboard zb;
	int i, s;

	br_size = 0;
	/* a BigRamPlus (4626/134) first; else the largest other memory board.
	 * Never an Elbox board: a Mediator's PCI window is not RAM. */
	for (i = 0; plat_zorro(i, &zb) == 0; i++) {
		if (!(zb.zb_type & ERTF_MEMLIST) || zb.zb_manuf == 2206 ||
		    zb.zb_pa < 0x40000000UL || zb.zb_pa >= 0x80000000UL || zb.zb_size < BR_MINSIZE)
			continue;
		if (zb.zb_manuf == 4626 && zb.zb_prod == 134) {
			br_pa = zb.zb_pa;
			br_size = zb.zb_size;
			break;
		}
		if (zb.zb_size > br_size) {
			br_pa = zb.zb_pa;
			br_size = zb.zb_size;
		}
	}
	if (br_size == 0) {
		printf("bigram: no Zorro III memory board above 1 GB\n");
		return ENXIO;
	}
	/* its row must be empty (the same entries as row 0, which AMIX leaves
	 * empty): S06bigram makes the node with this major */
	i = BR_FIRSTROW;
	if (i >= bdevcnt || bdevsw[i].d_open != bdevsw[0].d_open ||
	    bdevsw[i].d_strategy != bdevsw[0].d_strategy ||
	    bdevsw[i].d_size != bdevsw[0].d_size) {
		printf("bigram: block device row %d is taken\n", BR_FIRSTROW);
		return EBUSY;
	}
	if ((br_base = plat_iomap(br_pa, br_size)) == 0) {
		printf("bigram: cannot map %x, %d KB\n", (int)br_pa, (int)(br_size >> 10));
		return ENOMEM;
	}
	s = splhi();
	br_row = i;
	br_saved = bdevsw[i];
	bdevsw[i].d_open = bropen;
	bdevsw[i].d_close = brclose;
	bdevsw[i].d_strategy = brstrategy;
	bdevsw[i].d_print = brprint;
	bdevsw[i].d_size = brsize;
	bdevsw[i].d_flag = &br_flag;
	splx(s);
	printf("bigram: %d KB Zorro III RAM at %x, block major %d, %d blocks\n",
	    (int)(br_size >> 10), (int)br_pa, br_row, (int)(br_size >> 9));
	return 0;
}

static int
bigram_unload()
{
	int s = splhi();

	if (br_open) {
		splx(s);
		return EBUSY;
	}
	if (br_row >= 0)
		bdevsw[br_row] = br_saved;
	br_row = -1;
	splx(s);
	return 0;
}

MOD_MISC_WRAPPER(bigram, bigram_load, bigram_unload, "Zorro III RAM block device");
