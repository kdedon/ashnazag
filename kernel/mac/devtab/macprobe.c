/*
 * Disk root probe, run from io_start[] just before the root mount (the
 * SCSI bus can be used and processes may sleep).  For a dd root it
 * opens the root and swap slices, which reads their partition maps,
 * then sets rootfstype from the root superblock and the swap size to
 * the swap slice's size.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/buf.h"
#include "sys/conf.h"
#include "sys/open.h"
#include "sys/file.h"
#include "sys/cmn_err.h"
#include "sys/errno.h"
#include "vm/bootconf.h"
#include "macroot.h"

#define DD_BMAJ		18

extern dev_t rootdev, dumpdev;
extern char rootfstype[];
extern struct bootobj swapfile;
extern int biowait();
extern char *strcpy();

static struct buf probebuf;
static long probedata[2048 / sizeof (long)];

static int
rdslice(arg, blk, n, buf)
char *arg;
long blk;
int n;
unsigned char *buf;
{
	register struct buf *bp = &probebuf;
	dev_t dev = *(dev_t *)arg;
	int i;

	bp->b_flags = B_BUSY | B_READ | B_KERNBUF;
	bp->b_bcount = n;
	bp->b_blkno = blk;
	bp->b_edev = dev;
	bp->b_dev = cmpdev(dev);
	bp->b_un.b_addr = (caddr_t)probedata;
	bp->b_proc = 0;
	bp->b_resid = 0;
	bp->b_error = 0;
	bp->b_iodone = 0;
	(*bdevsw[getmajor(dev)].d_strategy)(bp);
	if (biowait(bp) || bp->b_resid)
		return EIO;
	for (i = 0; i < n; i++)
		buf[i] = ((unsigned char *)probedata)[i];
	return 0;
}

static int
slopen(dev)
dev_t dev;
{
	return (*bdevsw[getmajor(dev)].d_open)(&dev, FREAD, OTYP_LYR,
	    (struct cred *)0);
}

static void
slclose(dev)
dev_t dev;
{
	(void)(*bdevsw[getmajor(dev)].d_close)(dev, FREAD, OTYP_LYR,
	    (struct cred *)0);
}

void
mac_diskprobe()
{
	static unsigned char sb[2048];
	dev_t r = rootdev, s = dumpdev;
	char *fs;
	int n;

	if (getmajor(r) != DD_BMAJ)
		return;
	if (r == s)
		cmn_err(CE_PANIC, "root and swap on one slice (c%dd0s%d)",
		    getminor(r) & 7, getminor(r) >> 4 & 7);
	if (slopen(r)) {
		printf("mac: root c%dd0s%d cannot be opened\n",
		    getminor(r) & 7, getminor(r) >> 4 & 7);
		return;
	}
	fs = mac_fsprobe(rdslice, (char *)&r, sb);
	slclose(r);
	if (*fs)
		strcpy(rootfstype, fs);
	printf("mac: root c%dd0s%d %s", getminor(r) & 7, getminor(r) >> 4 & 7,
	    *fs ? fs : "(no s5/ufs superblock)");

	n = 0;
	if (getmajor(s) == DD_BMAJ && slopen(s) == 0) {
		n = (*bdevsw[getmajor(s)].d_size)(s);
		slclose(s);
	}
	if (n > swapfile.bo_offset) {
		swapfile.bo_size = n - swapfile.bo_offset;
		printf(", swap %s %d blocks\n", swapfile.bo_name,
		    swapfile.bo_size);
	} else
		printf(", swap %s missing\n", swapfile.bo_name);
}
