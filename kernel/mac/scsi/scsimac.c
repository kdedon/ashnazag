/*
 * Macintosh side of the AMIX SCSI layer (sd.h): host adapter selection
 * and disk slices from the Apple Partition Map.
 *
 * Replaces the Amiga sdopen/sdqueue/sdhardwarename (controller table
 * filled by autoconfig) and sdpartition/sdvalid/sdblkno/sddevsize (Rigid
 * Disk Block).
 *
 * Minor number (sd.h): bits 0-2 target, bit 3 card (only 0), bits 4-6
 * slice.  Slice 0 is the whole disk; 1..7 come from apm_scan().
 */

#include	"sys/types.h"
#include	"sys/param.h"
#include	"sys/buf.h"
#include	"sys/errno.h"
#include	"rico.h"
#include	"sd.h"
#include	"macspl.h"
#include	"apm.h"

#ifndef BSIZE
#define BSIZE 512
#endif

extern void	ncr96init(), ncr96queue();

struct partab {
	ulong	base,
		len;
};

static struct partab	partab[0x100];
static struct apm_slice	sl[APM_NSLICE];
static long		blkbuf[APM_BSIZE / sizeof (long)];
static struct buf	rbuf;
static bool		busy;
static bool		shown[SDCARDS * SDUNITS];
static int		(*rdstrat)();
static dev_t		rddev;

static char	hwname[] = "Quadra 53C96 SCSI";
static char	*hows[] = { "", "disk", "bzb slice", "bzb", "name", "unix", "other" };

int
sdopen(card)
{
	if (card != 0)
		return ENXIO;
	ncr96init(1);
	return 0;
}

void
sdqueue(cp)
struct sdcom	*cp;
{
	ncr96queue(cp);
}

char *
sdhardwarename(card)
uint	card;
{
	return hwname;
}

/* Read one block of the whole-disk device for apm_scan. */
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
	x = splscsi();
	while (!(rbuf.b_flags & B_DONE))
		sleep(&rbuf, PRIBIO);
	splrestore(x);
	if ((rbuf.b_flags & B_ERROR) || rbuf.b_resid)
		return rbuf.b_error ? rbuf.b_error : EIO;
	for (i = 0; i < BSIZE; i++)
		buf[i] = ((uchar *)blkbuf)[i];
	return 0;
}

static void
show(dev)
dev_t	dev;
{
	int	s;

	printf("%s: disk c%dd0:", hwname, sdcard(dev) * 8 + sdunit(dev));
	if (sl[0].len)
		printf(" %d blocks", sl[0].len);
	printf("\n");
	for (s = 1; s < APM_NSLICE; s++)
		if (sl[s].how)
			printf("  s%d: %d+%d \"%s\" %s (%s)\n", s, sl[s].base,
			    sl[s].len, sl[s].name, sl[s].type, hows[sl[s].how]);
}

/*
 * Called by the disk driver's open.  Reads the partition map every time,
 * so a relabelled disk is seen at the next open.
 */
int
sdpartition(dev, strat)
dev_t	dev;
int	(*strat)();
{
	uint	d0, s, u;
	int	error;

	while (busy)
		sleep(&busy, PRIBIO);
	busy = TRUE;
	rdstrat = strat;
	rddev = sddev0p(dev);
	error = apm_scan(rdblk, (char *)0, sl);
	d0 = MINOR(sddev0p(dev));
	if (error == 0) {
		for (s = 0; s < APM_NSLICE; s++) {
			partab[d0 | s << 4].base = sl[s].base;
			partab[d0 | s << 4].len = sl[s].len;
		}
		u = sdcard(dev) * SDUNITS + sdunit(dev);
		if (!shown[u]) {
			shown[u] = TRUE;
			show(dev);
		}
	} else if (error < 0)
		for (s = 1; s < APM_NSLICE; s++)
			partab[d0 | s << 4].len = 0;
	busy = FALSE;
	wakeup(&busy);
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
	if (!sdpart(bp->b_edev)
	|| bp->b_blkno + (bp->b_bcount + BSIZE - 1) / BSIZE <= partab[MINOR(bp->b_edev)].len)
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

/* Blocks in a slice; a large number for a disk whose size is unknown. */
uint
sddevsize(dev)
dev_t	dev;
{
	return partab[MINOR(dev)].len ? partab[MINOR(dev)].len : 1234567;
}
