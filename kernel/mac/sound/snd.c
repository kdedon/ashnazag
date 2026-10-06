/*
 * snd.c -- /dev/asc: the sound chip for the sound service.
 *
 * The service maps the chip and feeds it; the kernel only takes the
 * chip's interrupt (VIA2 CB1), which user code cannot, and reports it
 * with display switches as events.  Reading ASC_FIFOIRQ clears it, so
 * only the interrupt reads it.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/errno.h"
#include "sys/poll.h"
#include "sys/cred.h"
#include "sys/uio.h"
#include "sys/stream.h"
#include "ds.h"
#include "sndio.h"

#define VIA2		0x50F02000
#define VIA2_IFR	0x1A00
#define VIA2_IER	0x1C00
#define VIA_CB1		0x10
#define PRI		((PZERO + 1) | PCATCH)
#define ASC(r)		(((VOL unsigned char *)ASC_PHYS)[r])
#define VIA(r)		(((VOL unsigned char *)VIA2)[r])

extern int uiomove();
extern void pollwakeup();

struct sndev snd_ev;
static int snd_busy, snd_pend;
static struct pollhead snd_ph;

static void
snd_wake()
{
	snd_pend = 1;
	wakeup((caddr_t)&snd_ev);
	pollwakeup(&snd_ph, POLLIN | POLLRDNORM);
}

/* VIA2 CB1, IPL 2; the edge is acknowledged */
void
snd_intr()
{
	snd_ev.se_irq |= ASC(ASC_FIFOIRQ);
	snd_ev.se_nirq++;
	if (snd_busy)
		snd_wake();
}

/* from a display switch, at DS_HI */
static void
snd_front(s)
struct dssess *s;
{
	snd_ev.se_front = s->s_id;
	snd_ev.se_fuid = s->s_uid;
	snd_ev.se_serial++;
	snd_wake();
}

/* chip silent, its interrupt off */
static void
snd_quiet()
{
	ASC(ASC_MODE) = 0;
	ASC(ASC_IRQMASKA) = 1;
	ASC(ASC_IRQMASKB) = 1;
	VIA(VIA2_IER) = VIA_CB1;
	VIA(VIA2_IFR) = VIA_CB1;
}

/*ARGSUSED*/
int
snd_open(devp, flag, otyp, cr)
dev_t *devp;
int flag, otyp;
struct cred *cr;
{
	register int x;

	if (getminor(*devp) != 0)
		return ENXIO;
	if (drv_priv(cr))
		return EPERM;
	x = DS_SPL(DS_HI);
	if (snd_busy) {
		DS_SPLX(x);
		return EBUSY;
	}
	snd_busy = 1;
	snd_quiet();
	(void)ASC(ASC_FIFOIRQ);
	snd_ev.se_irq = 0;
	snd_ev.se_front = ds_front->s_id;
	snd_ev.se_fuid = ds_front->s_uid;
	snd_pend = 1;
	ds_frontfn = snd_front;
	VIA(VIA2_IER) = 0x80 | VIA_CB1;
	DS_SPLX(x);
	return 0;
}

/*ARGSUSED*/
int
snd_close(dev, flag, otyp, cr)
dev_t dev;
int flag, otyp;
struct cred *cr;
{
	register int x = DS_SPL(DS_HI);

	snd_quiet();
	ds_frontfn = 0;
	snd_busy = 0;
	DS_SPLX(x);
	return 0;
}

/*ARGSUSED*/
int
snd_read(dev, uio, cr)
dev_t dev;
struct uio *uio;
struct cred *cr;
{
	struct sndev e;
	register int x;

	if (uio->uio_resid < sizeof e)
		return EINVAL;
	x = DS_SPL(DS_HI);
	while (!snd_pend)
		if (sleep((caddr_t)&snd_ev, PRI)) {
			DS_SPLX(x);
			return EINTR;
		}
	e = snd_ev;
	snd_ev.se_irq = 0;
	snd_pend = 0;
	DS_SPLX(x);
	return uiomove((caddr_t)&e, sizeof e, UIO_READ, uio);
}

/*ARGSUSED*/
int
snd_poll(dev, events, anyyet, reventsp, phpp)
dev_t dev;
short events;
int anyyet;
short *reventsp;
struct pollhead **phpp;
{
	*reventsp = snd_pend ? events & (POLLIN | POLLRDNORM) : 0;
	if (*reventsp == 0 && !anyyet)
		*phpp = &snd_ph;
	return 0;
}

/* the register page, cache-inhibited as device memory is */
/*ARGSUSED*/
int
snd_mmap(dev, off, prot)
dev_t dev;
off_t off;
int prot;
{
	if (off < 0 || off >= ASC_SIZE)
		return -1;
	return (ASC_PHYS + off) >> DS_PGSHIFT;
}
