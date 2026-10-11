/*
 * The floppy service on an Amiga host: /dev/floppyN (major 62), raw
 * tracks through the host driver's own track routines.  The host
 * driver's open and close are wrapped so it and the claims exclude each
 * other; loaded at boot, before the host driver is first opened.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sys/conf.h"
#include "sys/open.h"
#include "sys/file.h"
#include "sys/moddefs.h"
#include "sys/sysmacros.h"
#include "sys/systm.h"
#include "sys/inline.h"
#include "flio.h"

#define PRA (*(volatile unsigned char *)0xbfe001)	/* CIA-A */
#define PRB (*(volatile unsigned char *)0xbfd100)	/* CIA-B */
#define CHNG (1 << 2)		/* CIA-A pra: low after a disk change */
#define STEP (1 << 0)		/* CIA-B prb, active low */
#define DIREC (1 << 1)		/* CIA-B prb: high steps out */
#define MEMF_CHIP 2
#define QUIET (3 * HZ)		/* the host driver's track cache drains */

extern int fdopen(), fdclose();
extern int flread(), flwrite(), fliwrite(), flwprot(), flok();
extern void flinit(), flstop(), fllost(), fldelay(), delay();
extern unsigned flid();
extern char *AllocMem();
extern void FreeMem();
extern clock_t lbolt;

static int fl_flag;
static char *fl_buf;			/* chip memory, for the DMA */
static int fl_busy;
static char fl_owner[FL_NDRIVE], fl_hd[FL_NDRIVE];
static short fl_disk[FL_NDRIVE];	/* -1 not looked yet */
static unsigned long fl_changes[FL_NDRIVE];
static int fl_claims, fl_inited;

/* the host driver: its entries, its minors open (1 char, 2 block) */
static struct cdevsw *fl_cd;
static struct bdevsw *fl_bd;
static int (*o_copen)(), (*o_cclose)(), (*o_bopen)(), (*o_bclose)();
static unsigned char fl_host[256];
static int fl_hostn, fl_inhook;
static clock_t fl_quiet;

static void
lock()
{
	while (fl_busy)
		sleep((caddr_t)&fl_busy, PRIBIO);
	fl_busy = 1;
}

static void
unlock()
{
	fl_busy = 0;
	wakeup((caddr_t)&fl_busy);
}

static int
hostopen(o, devp, flag, otyp, cr, bit)
	int (*o)();
	dev_t *devp;
	int flag, otyp, bit;
	struct cred *cr;
{
	int m = getminor(*devp), e, had;

	/* minor 255 is the driver's servant: it touches drives only for opened minors */
	if (m == 255) {
		fl_inhook++;
		e = (*o)(devp, flag, otyp, cr);
		fl_inhook--;
		return e;
	}
	if (fl_claims)
		return EBUSY;
	had = fl_host[m];
	if (!had)
		fl_hostn++;
	fl_host[m] |= bit;
	fl_inhook++;
	e = (*o)(devp, flag, otyp, cr);
	fl_inhook--;
	if (e && !(had & bit)) {
		fl_host[m] = had;
		if (!had)
			fl_hostn--;
	}
	return e;
}

static int
hostclose(o, dev, flag, otyp, cr, bit)
	int (*o)();
	dev_t dev;
	int flag, otyp, bit;
	struct cred *cr;
{
	int m = getminor(dev), e;

	fl_inhook++;
	e = (*o)(dev, flag, otyp, cr);
	fl_inhook--;
	if (m != 255 && fl_host[m]) {
		fl_host[m] &= ~bit;
		if (!fl_host[m]) {
			fl_hostn--;
			fl_quiet = lbolt + QUIET;
		}
	}
	return e;
}

static int h_copen(d, f, t, c) dev_t *d; int f, t; struct cred *c; { return hostopen(o_copen, d, f, t, c, 1); }
static int h_bopen(d, f, t, c) dev_t *d; int f, t; struct cred *c; { return hostopen(o_bopen, d, f, t, c, 2); }
static int h_cclose(d, f, t, c) dev_t d; int f, t; struct cred *c; { return hostclose(o_cclose, d, f, t, c, 1); }
static int h_bclose(d, f, t, c) dev_t d; int f, t; struct cred *c; { return hostclose(o_bclose, d, f, t, c, 2); }

/* 0 no drive, else its id: DF0 answers 0 though present */
static unsigned
present(drv)
	int drv;
{
	unsigned id = flid(drv);

	return drv == 0 && id == 0 ? ~0U : id;
}

static int
flopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	int drv = getminor(*devp), e = 0;
	unsigned id;

	if (drv >= FL_NDRIVE)
		return ENXIO;
	lock();
	while (!fl_hostn && fl_quiet - lbolt > 0)
		delay(fl_quiet - lbolt);
	if (fl_owner[drv] || fl_hostn)
		e = EBUSY;
	else {
		if (!fl_inited) {
			flinit();
			fl_inited = 1;
		}
		if ((id = present(drv)) == 0)
			e = ENXIO;
		else {
			fl_owner[drv] = 1;
			fl_claims++;
			fl_disk[drv] = -1;
			fl_hd[drv] = id == 0xaaaaaaaaU;
			fl_changes[drv] = 0;
		}
	}
	unlock();
	return e;
}

static int
flclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	int drv = getminor(dev);

	lock();
	flstop(drv);
	fl_owner[drv] = 0;
	fl_claims--;
	unlock();
	return 0;
}

/* CHNG stays low after a change until a step with a disk in: step in and back out */
static int
diskin(drv, swapped)
	int drv, *swapped;
{
	unsigned char prb;
	int i;

	*swapped = 0;
	flwprot(drv);			/* selects the drive */
	if (PRA & CHNG)
		return 1;
	prb = PRB | STEP;
	for (i = 0; i < 2; i++) {
		prb = i ? prb | DIREC : prb & ~DIREC;
		PRB = prb;
		PRB = prb & ~STEP;
		fldelay(1);		/* shorter pulses are missed */
		PRB = prb;
		fldelay(3000);
	}
	fldelay(15000);
	fllost(drv);
	return *swapped = (PRA & CHNG) != 0;
}

static int
info(drv, fi)
	int drv;
	struct flinfo *fi;
{
	int in, swapped;
	unsigned id;

	in = diskin(drv, &swapped);
	if (in != fl_disk[drv] || swapped) {
		fl_changes[drv]++;
		fl_disk[drv] = in;
		/* an HD drive tells its density by the disk in it */
		if (in && (id = present(drv)) != 0)
			fl_hd[drv] = id == 0xaaaaaaaaU;
	}
	fi->fi_flags = (in ? FLI_DISK : 0) | (in && flwprot(drv) ? FLI_WPROT : 0) |
	    (fl_hd[drv] ? FLI_HD : 0);
	fi->fi_enc = FLE_MFM;
	fi->fi_tracks = 160;
	fi->fi_rawlen = fl_hd[drv] ? 25336 : 12668;
	fi->fi_changes = fl_changes[drv];
	return 0;
}

static int
raw(drv, cmd, arg)
	int drv, cmd;
	caddr_t arg;
{
	struct flraw fr;
	int ok;

	if (copyin(arg, (caddr_t)&fr, sizeof fr))
		return EFAULT;
	if (fr.fr_track >= 160 || fr.fr_len == 0 || fr.fr_len > FL_MAXRAW || (fr.fr_len & 1))
		return EINVAL;
	if (cmd == FLIOC_READ) {
		if (!flread(drv, fr.fr_track, fl_buf, fr.fr_len))
			return EIO;
		return copyout(fl_buf, fr.fr_buf, fr.fr_len) ? EFAULT : 0;
	}
	if (copyin(fr.fr_buf, fl_buf, fr.fr_len))
		return EFAULT;
	if (fr.fr_flags & FLR_INDEX)
		ok = fliwrite(drv, fr.fr_track, fl_buf, fr.fr_len);
	else
		ok = flwrite(drv, fr.fr_track, fl_buf, fr.fr_len);
	if (!ok)
		return flwprot(drv) ? EROFS : EIO;
	return 0;
}

static int
flioctl(dev, cmd, arg, mode, cr, rvalp)
	dev_t dev;
	int cmd, mode;
	caddr_t arg;
	struct cred *cr;
	int *rvalp;
{
	int drv = getminor(dev), e;
	struct flinfo fi;

	lock();
	switch (cmd) {
	case FLIOC_INFO:
		e = info(drv, &fi);
		if (!e && copyout((caddr_t)&fi, arg, sizeof fi))
			e = EFAULT;
		break;
	case FLIOC_READ:
		e = raw(drv, cmd, arg);
		break;
	case FLIOC_WRITE:
		e = (mode & FWRITE) ? raw(drv, cmd, arg) : EBADF;
		break;
	case FLIOC_MOTOR:
		e = 0;
		if (arg)
			e = flok(drv) ? 0 : EIO;
		else
			flstop(drv);
		break;
	default:
		e = EINVAL;
	}
	unlock();
	return e;
}

struct mod_drv_data floppy_drvdata[] = {
	{ { 0 }, 0, 0,
	  { flopen, flclose, 0, 0, flioctl, 0, 0, 0, 0, 0, 0, 0, &fl_flag },
	  FL_MAJOR, 1 }
};

static int
floppy_load()
{
	int i;

	for (i = 0; i < cdevcnt; i++)
		if (cdevsw[i].d_open == fdopen)
			fl_cd = &cdevsw[i];
	for (i = 0; i < bdevcnt; i++)
		if (bdevsw[i].d_open == fdopen)
			fl_bd = &bdevsw[i];
	if ((fl_buf = AllocMem(FL_MAXRAW + 2, MEMF_CHIP)) == 0)
		return ENOMEM;
	i = splhi();
	if (fl_cd) {
		o_copen = fl_cd->d_open; o_cclose = fl_cd->d_close;
		fl_cd->d_open = h_copen; fl_cd->d_close = h_cclose;
	}
	if (fl_bd) {
		o_bopen = fl_bd->d_open; o_bclose = fl_bd->d_close;
		fl_bd->d_open = h_bopen; fl_bd->d_close = h_bclose;
	}
	splx(i);
	return 0;
}

static int
floppy_unload()
{
	int s = splhi();

	if (fl_claims || fl_hostn || fl_inhook) {
		splx(s);
		return EBUSY;
	}
	if (fl_cd) {
		fl_cd->d_open = o_copen; fl_cd->d_close = o_cclose;
	}
	if (fl_bd) {
		fl_bd->d_open = o_bopen; fl_bd->d_close = o_bclose;
	}
	splx(s);
	FreeMem(fl_buf, FL_MAXRAW + 2);
	return 0;
}

MOD_DRV_WRAPPER(floppy, floppy_load, floppy_unload, 0, "floppy service");
