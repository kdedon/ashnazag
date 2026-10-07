/*
 * SCSI disks as card 1 of the disk driver: its commands go to the target
 * whose ID is the unit number.  Disks with 512-byte blocks only.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sys/inline.h"
#include "sys/moddefs.h"
#include "rico.h"
#include "sd.h"
#include "sdhook.h"
#include "scsi.h"

#define T_DISK		10000000L	/* us between REQs: a disk spinning up */
#define RETRIES		3

struct sdunit {
	bool	present;
	ulong	nblk;
	int	senselen;	/* sense kept for the driver's REQUEST SENSE */
	uchar	sense[18];
};

static struct sdunit	un[SC_MYID];
static struct sdcom	*qhead, *qtail;
static bool		running;
static struct scsi_job	job;

extern void	bcopy(), bzero();

static int
sd_cmd(u, cdb, n, buf, len, dir)
int u, n, dir;
uchar *cdb, *buf;
long len;
{
	bzero((caddr_t)&job, sizeof job);
	job.sj_target = u;
	bcopy((caddr_t)cdb, (caddr_t)job.sj_cdb, n);
	job.sj_cdblen = n;
	job.sj_data = buf;
	job.sj_len = len;
	job.sj_dir = dir;
	job.sj_timeout = T_DISK;
	job.sj_retries = RETRIES;
	return scsi_run(&job);
}

/* INQUIRY, TEST UNIT READY (starting the motor if need be), READ CAPACITY. */
static int
sd_probe1(u)
int u;
{
	static uchar c[10], d[36];
	int i, r;

	bzero((caddr_t)c, sizeof c);
	c[0] = 0x12;
	c[4] = sizeof d;
	if (sd_cmd(u, c, 6, d, (long)sizeof d, FS_IN) != SS_GOOD || job.sj_done < 5 ||
	    (d[0] & 0xe0) != 0 || ((d[0] & 0x1f) != 0 && (d[0] & 0x1f) != 7))
		return ENXIO;
	for (i = 0;; i++) {
		bzero((caddr_t)c, sizeof c);
		if ((r = sd_cmd(u, c, 6, (uchar *)0, 0L, FS_NONE)) == SS_GOOD)
			break;
		if (r != SS_CHECK || i > 0 || job.sj_senselen < 3 ||
		    (job.sj_sense[2] & 0x0f) != SK_NOTREADY)
			return EIO;
		c[0] = 0x1b;	/* START UNIT */
		c[4] = 1;
		(void)sd_cmd(u, c, 6, (uchar *)0, 0L, FS_NONE);
	}
	bzero((caddr_t)c, sizeof c);
	c[0] = 0x25;
	if (sd_cmd(u, c, 10, d, 8L, FS_IN) != SS_GOOD || job.sj_done < 8 ||
	    (d[4] | d[5] | d[7]) != 0 || d[6] != 2)
		return EIO;
	un[u].nblk = ((ulong)d[0] << 24 | (ulong)d[1] << 16 | d[2] << 8 | d[3]) + 1;
	un[u].senselen = 0;
	un[u].present = TRUE;
	return 0;
}

/* At sdspl, as the queue: they share job, and the IPL 1 network service waits. */
static int
sd_probe(u)
int u;
{
	int x, r;

	if (u >= SC_MYID || scsi_attach() < 0)
		return ENXIO;
	if (un[u].present)
		return 0;
	x = sdspl();
	r = sd_probe1(u);
	splx(x);
	return r;
}

static ulong
sd_nblk(u)
int u;
{
	return un[u].nblk;
}

static int
sd_rdblk(u, bn, buf)
int u;
long bn;
uchar *buf;
{
	static uchar c[10];
	int x, r;

	x = sdspl();
	bzero((caddr_t)c, sizeof c);
	c[0] = 0x28;
	c[2] = bn >> 24;
	c[3] = bn >> 16;
	c[4] = bn >> 8;
	c[5] = bn;
	c[8] = 1;
	r = sd_cmd(u, c, 10, buf, 512L, FS_IN) == SS_GOOD && job.sj_done == 512 ? 0 : EIO;
	splx(x);
	return r;
}

/*
 * One driver command.  The sense fetched on CHECK CONDITION answers the
 * driver's REQUEST SENSE that follows.  A disk that stops answering is
 * probed again at the next open.
 */
static void
sd_one(cp)
struct sdcom *cp;
{
	struct sdunit *up = &un[cp->unit];
	static int len[8] = { 6, 10, 10, 6, 6, 12, 6, 6 };
	int r;

	cp->okay = FALSE;
	cp->status = 0xFF;
	if (cp->unit >= SC_MYID || !up->present)
		return;
	if (cp->cdb[0] == 0x03 && up->senselen) {
		r = cp->nbyte < up->senselen ? cp->nbyte : up->senselen;
		bcopy((caddr_t)up->sense, cp->addr, r);
		up->senselen = 0;
		cp->status = SS_GOOD;
		cp->okay = TRUE;
		return;
	}
	r = sd_cmd((int)cp->unit, cp->cdb, len[cp->cdb[0] >> 5], (uchar *)cp->addr,
	    (long)cp->nbyte, cp->nbyte == 0 ? FS_NONE : cp->reading ? FS_IN : FS_OUT);
	up->senselen = 0;
	if (r == FS_NOTARGET)
		up->present = FALSE;
	if (r < 0 || (r == SS_GOOD && job.sj_done < cp->nbyte &&
	    (cp->cdb[0] == 0x28 || cp->cdb[0] == 0x2a)))
		return;
	if (r == SS_CHECK && job.sj_senselen) {
		up->senselen = job.sj_senselen;
		bcopy((caddr_t)job.sj_sense, (caddr_t)up->sense, up->senselen);
	}
	cp->status = r;
	cp->okay = TRUE;
}

/* As the IDE queue: each command runs to the end; completions never nest. */
static void
sd_queue(cp)
struct sdcom *cp;
{
	int x;

	x = sdspl();
	cp->next = 0;
	if (qhead)
		qtail->next = cp;
	else
		qhead = cp;
	qtail = cp;
	if (!running) {
		running = TRUE;
		while ((cp = qhead) != 0) {
			qhead = (struct sdcom *)cp->next;
			sd_one(cp);
			(*cp->intr)(cp);
		}
		running = FALSE;
	}
	splx(x);
}

static struct sdhook sd_hook = { "SCSI", sd_probe, sd_nblk, sd_rdblk, sd_queue };

static int
sd_load()
{
	ata_sdscsi = &sd_hook;
	return 0;
}

/* The disk driver does not report closes, so a loaded sd stays. */
static int
sd_unload()
{
	return EBUSY;
}

MOD_MISC_WRAPPER(sd, sd_load, sd_unload, "SCSI disk");
