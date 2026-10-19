/*
 * Falcon IDE as the ataide module, card 0 of the AMIX disk driver
 * (sd.h): its SCSI commands run as polled ATA PIO commands with 28-bit
 * LBA, unit 0 the master, 1 the slave.  It serves the disks, so it
 * never unloads.
 */

#include	"sys/types.h"
#include	"sys/param.h"
#include	"sys/inline.h"
#include	"sys/errno.h"
#include	"sys/moddefs.h"
#include	"amiga/alien/rico.h"
#include	"amiga/alien/sd.h"

extern int	printf(), ata_busprobe(), sd_attach();
extern void	delayus(), dlm_cachepush();

#ifndef BSIZE
#define BSIZE 512
#endif

/* Registers sit on odd bytes, 4 apart; the data register is 16 bits. */
#define IDE_DATA	(*(__volatile__ ushort *)0xFFF00000)
#define IDE_REG(n)	(*(__volatile__ uchar *)(0xFFF00001 + 4 * (n)))
#define IDE_ERR		IDE_REG(1)
#define IDE_NSECT	IDE_REG(2)
#define IDE_LBA0	IDE_REG(3)
#define IDE_LBA1	IDE_REG(4)
#define IDE_LBA2	IDE_REG(5)
#define IDE_SEL		IDE_REG(6)
#define IDE_CMD		IDE_REG(7)	/* status on read */
#define IDE_CTL		(*(__volatile__ uchar *)0xFFF00039)	/* alt status on read */

#define ST_ERR		0x01
#define ST_DRQ		0x08
#define ST_DF		0x20
#define ST_DRDY		0x40
#define ST_BSY		0x80

#define CTL_NIEN	0x02
#define CTL_SRST	0x04

#define ATA_READ	0x20
#define ATA_WRITE	0x30
#define ATA_IDENTIFY	0xEC

#define TIMEOUT		3000000		/* 10 us polls: 30 s */
#define NUNIT		2

/* sense keys */
#define SK_NOTREADY	0x02
#define SK_MEDIUM	0x03
#define SK_ILLEGAL	0x05

struct unit {
	bool	probed,
		present;
	ulong	nblk;
	uchar	sense[3];		/* key, ASC, ASCQ */
	char	model[41];
};

static struct unit	un[NUNIT];
static bool		reset;
static bool		noide;		/* no interface: bus error */
static struct sdcom	*qhead, *qtail;
static bool		running;

static char	hwname[] = "Falcon IDE";

/* Wait until BSY is clear; then until all of want are set, if any. */
static int
ide_wait(want)
int want;
{
	register long n;
	register int st;

	for (n = 0; n < TIMEOUT; n++) {
		st = IDE_CTL;
		if (!(st & ST_BSY)) {
			st = IDE_CMD;
			if (st & (ST_ERR | ST_DF))
				return -1;
			if ((st & want) == want)
				return 0;
		}
		delayus(10);
	}
	return -2;
}

static int
ide_select(u, lba)
int u;
ulong lba;
{
	if (ide_wait(0) == -2)
		return -1;
	IDE_SEL = 0xE0 | u << 4 | (lba >> 24 & 0x0F);
	delayus(1);
	return ide_wait(0) == -2 ? -1 : 0;
}

/*
 * Move n blocks between the unit and buf.  Data words go through as
 * they are: the bus swaps bytes, so the disk image keeps TOS order.
 */
static int
ide_rw(u, lba, buf, n, reading)
int u;
ulong lba;
ushort *buf;
ulong n;
bool reading;
{
	register ushort *p;
	register int i;
	ulong k;

	p = buf;
	while (n > 0) {
		k = n > 256 ? 256 : n;
		if (ide_select(u, lba) != 0)
			return -1;
		IDE_NSECT = k & 0xFF;
		IDE_LBA0 = lba;
		IDE_LBA1 = lba >> 8;
		IDE_LBA2 = lba >> 16;
		IDE_CMD = reading ? ATA_READ : ATA_WRITE;
		lba += k;
		n -= k;
		while (k-- > 0) {
			delayus(1);
			if (ide_wait(ST_DRQ) != 0)
				return -1;
			if (reading)
				for (i = BSIZE / 2; i > 0; i--)
					*p++ = IDE_DATA;
			else
				for (i = BSIZE / 2; i > 0; i--)
					IDE_DATA = *p++;
		}
		delayus(1);
		if (ide_wait(0) != 0)
			return -1;
	}
	return 0;
}

/* Resets both units; after a failed command a unit may still want data. */
static void
ide_reset()
{
	IDE_CTL = CTL_NIEN | CTL_SRST;
	delayus(10);
	IDE_CTL = CTL_NIEN;
	delayus(2000);
}

/* IDENTIFY: words arrive as values; strings are big-endian pairs. */
static void
ide_probe(u)
int u;
{
	struct unit *up = &un[u];
	static ushort id[256];
	register int i;

	up->probed = TRUE;
	up->present = FALSE;
	if (!reset) {
		reset = TRUE;
		noide = !ata_busprobe(&IDE_CTL);
		if (!noide)
			ide_reset();
	}
	if (noide || IDE_CTL == 0xFF || ide_select(u, 0L) != 0 || (IDE_SEL & 0x10) != u << 4
	|| !(IDE_CMD & ST_DRDY))
		return;
	IDE_CMD = ATA_IDENTIFY;
	delayus(1);
	if (ide_wait(ST_DRQ) != 0)
		return;
	for (i = 0; i < 256; i++)
		id[i] = IDE_DATA;
	for (i = 0; i < 20; i++) {
		up->model[2 * i] = id[27 + i] >> 8;
		up->model[2 * i + 1] = id[27 + i];
	}
	for (i = 40; i > 0 && (up->model[i - 1] == ' ' || up->model[i - 1] == 0); i--)
		;
	up->model[i] = 0;
	if (!(id[49] & 0x200))
		return;			/* no LBA */
	up->nblk = (ulong)id[61] << 16 | id[60];
	up->present = up->nblk != 0;
}

static void
setsense(up, key, asc)
struct unit *up;
int key, asc;
{
	up->sense[0] = key;
	up->sense[1] = asc;
	up->sense[2] = 0;
}

static void
fill(p, n, c)
uchar *p;
int n, c;
{
	while (n-- > 0)
		*p++ = c;
}

/* Run one SCSI command; returns the SCSI status. */
static int
ide_scsi(cp)
struct sdcom *cp;
{
	struct unit *up = &un[cp->unit];
	uchar *c = cp->cdb, *d = (uchar *)cp->addr;
	ulong lba, n;
	int i;

	switch (c[0]) {
	case 0x00:			/* TEST UNIT READY */
		return 0;
	case 0x03:			/* REQUEST SENSE */
		n = cp->nbyte < 14 ? cp->nbyte : 14;
		fill(d, (int)n, 0);
		if (n > 0) d[0] = 0x70;
		if (n > 2) d[2] = up->sense[0];
		if (n > 7) d[7] = 6;
		if (n > 12) d[12] = up->sense[1];
		if (n > 13) d[13] = up->sense[2];
		setsense(up, 0, 0);
		return 0;
	case 0x12:			/* INQUIRY */
		n = cp->nbyte < 36 ? cp->nbyte : 36;
		fill(d, (int)n, ' ');
		for (i = 0; i < 8 && i < n; i++)
			d[i] = "\0\0\2\2\37\0\0\0"[i];
		for (i = 0; i < 4 && 8 + i < n; i++)
			d[8 + i] = "ATA "[i];
		for (i = 0; i < 16 && up->model[i] && 16 + i < n; i++)
			d[16 + i] = up->model[i];
		return 0;
	case 0x25:			/* READ CAPACITY */
		if (cp->nbyte < 8)
			break;
		n = up->nblk - 1;
		d[0] = n >> 24; d[1] = n >> 16; d[2] = n >> 8; d[3] = n;
		d[4] = 0; d[5] = 0; d[6] = BSIZE >> 8; d[7] = 0;
		return 0;
	case 0x08:			/* READ(6), WRITE(6) */
	case 0x0A:
		lba = (ulong)(c[1] & 0x1F) << 16 | c[2] << 8 | c[3];
		n = c[4] ? c[4] : 256;
		goto rw;
	case 0x28:			/* READ(10), WRITE(10) */
	case 0x2A:
		lba = (ulong)c[2] << 24 | (ulong)c[3] << 16 | c[4] << 8 | c[5];
		n = c[7] << 8 | c[8];
	rw:
		if (lba >= up->nblk || n > up->nblk - lba || lba + n > 0x10000000
		|| n * BSIZE > cp->nbyte || ((long)d & 1)) {
			setsense(up, SK_ILLEGAL, 0x21);
			return 2;
		}
		if (ide_rw(cp->unit, lba, (ushort *)d, n, c[0] == 0x08 || c[0] == 0x28)) {
			printf("%s: unit %d: %s error, block %d, status 0x%x error 0x%x\n",
			    hwname, cp->unit, (c[0] & 2) ? "write" : "read",
			    (int)lba, IDE_CMD, IDE_ERR);
			ide_reset();
			setsense(up, SK_MEDIUM, (c[0] & 2) ? 0x0C : 0x11);
			return 2;
		}
		return 0;
	}
	setsense(up, SK_ILLEGAL, 0x20);
	return 2;
}

/*
 * Runs each command to the end and completes it.  A command queued by a
 * completion runs once that completion returns, so they never nest.
 */
static bool
ide_queue(c, cp)
uint	c;
struct sdcom	*cp;
{
	int	x;

	/* the transfer runs through the uncached physical window */
	dlm_cachepush(cp->addr, cp->nbyte);
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
			cp->okay = FALSE;
			cp->status = 0xFF;
			if (cp->unit < NUNIT) {
				if (!un[cp->unit].probed)
					ide_probe((int)cp->unit);
				if (un[cp->unit].present) {
					cp->status = ide_scsi(cp);
					cp->okay = TRUE;
				}
			}
			(*cp->intr)(cp);
		}
		running = FALSE;
	}
	splx(x);
	return TRUE;
}

static int
ataide_load()
{
	if (!ata_busprobe(&IDE_CTL))
		return ENXIO;
	return sd_attach((int (*)())0, ide_queue, 0, (char *)0xFFF00000, hwname);
}

static int
ataide_unload()
{
	return EBUSY;
}

MOD_MISC_WRAPPER(ataide, ataide_load, ataide_unload, "Falcon IDE");
