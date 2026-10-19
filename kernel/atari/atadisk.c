/*
 * Slices for the AMIX disk driver (sd.h) from the AHDI root sector.
 * Controllers attach through sd_attach: IDE as card 0, SCSI as card 1.
 *
 * Minor number (sd.h): bits 0-2 unit (IDE: 0 master, 1 slave; SCSI: the
 * ID), bit 3 card, bits 4-6 slice.
 */

#include	"sys/types.h"
#include	"sys/param.h"
#include	"sys/buf.h"
#include	"sys/inline.h"
#include	"sys/errno.h"
#include	"rico.h"
#include	"sd.h"
#include	"ahdi.h"
#include	"vm/bootconf.h"

extern int	printf(), sleep(), sd_attach(), sdopen();
extern void	wakeup(), iodone(), sdqueue();
extern char	*sdhardwarename();

#ifndef BSIZE
#define BSIZE 512
#endif

#define IDE_ADDR	((char *)0xFFF00000)

struct partab {
	ulong	base,
		len;
};

static struct partab	partab[0x100];
static struct ahdi_slice sl[AHDI_NSLICE];
static long		blkbuf[BSIZE / sizeof (long)];
static struct buf	rbuf;
static bool		busy;
#define DISK(dev)	((dev) & (SDCARDS * SDUNITS - 1))	/* card and unit */
static bool		shown[SDCARDS * SDUNITS];
static bool		scanned[SDCARDS * SDUNITS];
static int		(*rdstrat)();
static dev_t		rddev;

extern dev_t		dumpdev;
extern struct bootobj	swapfile;

/* the controller modules the disk driver loads for a card with none */
char	*sd_modnames[] = { "ataide", "sd", 0 };

/* card 0 stays IDE's, so SCSI is card 1 with or without it */
void
sd_hostinit()
{
	(void)sd_attach((int (*)())0, (bool (*)())0, 0, IDE_ADDR, "IDE");
}

static bool		rcend;

static void
rcdone(cp)
struct sdcom	*cp;
{
	rcend = TRUE;
	wakeup((caddr_t)cp);
}

/* READ CAPACITY through the controller: blocks, 0 if no disk answers */
static ulong
capacity(dev)
dev_t	dev;
{
	static struct sdcom	c;
	static uchar		d[8];
	int	i, x;

	if (sdopen(sdcard(dev)) != 0)
		return 0;
	for (i = 0; i < sizeof c.cdb; i++)
		c.cdb[i] = 0;
	c.cdb[0] = 0x25;
	c.reading = TRUE;
	c.addr = (caddr_t)d;
	c.nbyte = sizeof d;
	c.card = sdcard(dev);
	c.unit = sdunit(dev);
	c.intr = rcdone;
	rcend = FALSE;
	x = sdspl();
	sdqueue(&c);
	while (!rcend)
		sleep((caddr_t)&c, PRIBIO);
	splx(x);
	if (!c.okay || c.status != 0 || (d[4] | d[5] | d[7]) != 0 || d[6] != 2)
		return 0;
	return ((ulong)d[0] << 24 | (ulong)d[1] << 16 | d[2] << 8 | d[3]) + 1;
}

/* One block of the whole disk through the driver, for ahdi_scan. */
static int
rdblk(arg, bn, buf)
char	*arg;
long	bn;
uchar	*buf;
{
	int	x, i;

	rbuf.b_flags = B_READ | B_KERNBUF;
	rbuf.b_bcount = BSIZE;
	rbuf.b_blkno = bn;
	rbuf.b_edev = rddev;
	rbuf.b_un.b_addr = (caddr_t)blkbuf;
	rbuf.b_proc = 0;
	rbuf.b_resid = 0;
	rbuf.b_error = 0;
	(*rdstrat)(&rbuf);
	x = sdspl();
	while (!(rbuf.b_flags & B_DONE))
		sleep((caddr_t)&rbuf, PRIBIO);
	splx(x);
	if ((rbuf.b_flags & B_ERROR) || rbuf.b_resid)
		return rbuf.b_error ? rbuf.b_error : EIO;
	for (i = 0; i < BSIZE; i++)
		buf[i] = ((uchar *)blkbuf)[i];
	return 0;
}

static void
show(dev, nblk)
dev_t	dev;
ulong	nblk;
{
	int	s;

	printf("%s: disk c%dd0: %d blocks\n", sdhardwarename(sdcard(dev)),
	    (int)(sdcard(dev) * 8 + sdunit(dev)), (int)nblk);
	for (s = 1; s < AHDI_NSLICE; s++)
		if (sl[s].len)
			printf("  s%d: %d+%d %s\n", s, (int)sl[s].base, (int)sl[s].len, sl[s].id);
}

/*
 * Called by the disk driver's open.  Reads the root sector every time,
 * so a repartitioned disk is seen at the next open.
 */
int
sdpartition(dev, strat)
dev_t	dev;
int	(*strat)();
{
	uint	d0, s;
	ulong	nblk;
	int	error;

	while (busy)
		sleep((caddr_t)&busy, PRIBIO);
	busy = TRUE;
	d0 = MINOR(sddev0p(dev));
	nblk = capacity(dev);
	error = ENXIO;
	if (nblk) {
		partab[d0].base = 0;
		partab[d0].len = nblk;
		rdstrat = strat;
		rddev = sddev0p(dev);
		error = ahdi_scan(rdblk, (char *)0, sl);
	}
	if (error == 0) {
		if (sl[0].len == 0 || sl[0].len > nblk)
			sl[0].len = nblk;
		for (s = 1; s < AHDI_NSLICE; s++)
			if (sl[s].base > nblk || sl[s].len > nblk - sl[s].base)
				sl[s].len = 0;
		for (s = 0; s < AHDI_NSLICE; s++) {
			partab[d0 | s << 4].base = sl[s].base;
			partab[d0 | s << 4].len = sl[s].len;
		}
		if (!shown[DISK(dev)]) {
			shown[DISK(dev)] = TRUE;
			show(dev, nblk);
		}
		/* swap on this disk: use its whole slice */
		if (sddev0p(dumpdev) == sddev0p(dev) && partab[MINOR(dumpdev)].len)
			swapfile.bo_size = partab[MINOR(dumpdev)].len;
	} else {
		partab[d0].base = 0;
		partab[d0].len = nblk;
		for (s = 1; s < AHDI_NSLICE; s++)
			partab[d0 | s << 4].len = 0;
	}
	scanned[DISK(dev)] = nblk != 0;
	busy = FALSE;
	wakeup((caddr_t)&busy);
	if (nblk == 0)
		return ENXIO;
	if (sdpart(dev) == 0)
		return 0;
	if (error > 0)
		return error;
	if (error < 0 || partab[MINOR(dev)].len == 0)
		return ENXIO;
	return 0;
}

bool
sdvalid(bp)
struct buf	*bp;
{
	if (bp->b_blkno >= 0 && bp->b_blkno + (bp->b_bcount + BSIZE - 1) / BSIZE
	    <= partab[MINOR(bp->b_edev)].len)
		return TRUE;
	bp->b_resid = bp->b_bcount;
	bp->b_flags |= B_ERROR;
	iodone(bp);
	return FALSE;
}

uint
sdblkno(bp)
struct buf	*bp;
{
	return partab[MINOR(bp->b_edev)].base + bp->b_blkno;
}

uint
sddevsize(dev)
dev_t	dev;
{
	/*
	 * A block device's size is taken when its node is looked up, which
	 * can be before the open that reads the slices: until then, the most
	 * a byte offset can address.  sdvalid still bounds every transfer.
	 */
	if (!scanned[DISK(dev)])
		return 0x3fffff;
	return partab[MINOR(dev)].len;
}
