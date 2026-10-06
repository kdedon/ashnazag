/*
 * auxsnd.c -- /dev/snd, the A/UX Sound Manager's device, relayed to
 * the sound service (sndio.h).  The service opens minor SA_SRV, reads
 * requests and writes replies; the caller sleeps until its reply comes.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/errno.h"
#include "sys/poll.h"
#include "sys/cred.h"
#include "sys/uio.h"
#include "sys/immu.h"
#include "sys/proc.h"
#include "sys/user.h"
#include "sndio.h"

#define SA_NCH		(SA_NSYN + 1)	/* the last is SA_RESET */
#define WAITPRI		PZERO		/* the service answers at once */
#define WAITTMO		(5 * 60)	/* ticks (60 Hz) before a silent service's caller gives up */
#define SPACEPRI	((PZERO + 1) | PCATCH)
#define IOCSIZE(c)	(((c) >> 16) & 0x1fff)
#define IOCIN		0x80000000
#define IOCOUT		0x40000000

extern int uiomove(), copyin(), copyout(), sleep(), drv_priv();
extern int timeout();
extern void wakeup(), bcopy(), bzero(), untimeout();
extern void pollwakeup();

/* a_state */
#define IDLE		0
#define POSTED		1
#define TAKEN		2
#define ANSWERED	3

static struct sach {
	int		a_open;
	int		a_busy;		/* a request is in flight */
	int		a_state;
	int		a_space;	/* SAP_SPACE came */
	int		a_rd;		/* the service is copying to or from a_buf */
	int		a_tmo;		/* the caller's wait ran out */
	int		a_gaveup;	/* the last call was abandoned */
	struct sareq	a_q;
	struct sarep	a_p;
	char		a_buf[SA_MAX];	/* the request's bytes, then the reply's */
} sa_ch[SA_NCH];

static int sa_srv;
static unsigned long sa_seq;
static struct pollhead sa_ph;

static struct sach *
chan(dev)
dev_t dev;
{
	register int m = getminor(dev);

	return m < SA_NSYN ? &sa_ch[m] : m == SA_RESET ? &sa_ch[SA_NSYN] : 0;
}

static void
sa_tmo(a)
register struct sach *a;
{
	a->a_tmo = 1;
	wakeup((caddr_t)a);
}

/* an errno from a reply, or EIO when the service sends nonsense */
static int
errn(r)
long r;
{
	return r < 0 && r >= -255 ? (int)-r : EIO;
}

static void
acquire(a)
register struct sach *a;
{
	while (a->a_busy)
		(void)sleep((caddr_t)&a->a_busy, WAITPRI);
	a->a_busy = 1;
}

static void
release(a)
register struct sach *a;
{
	a->a_busy = 0;
	wakeup((caddr_t)&a->a_busy);
}

/*
 * Post a request with len bytes in a_buf, a acquired; the reply's result.
 * A caller that may be interrupted leaves on a signal (-EINTR); every caller
 * leaves after WAITTMO ticks without a reply (-EIO), so a hung service never
 * wedges the Mac.  A late reply is refused: the state is no longer TAKEN.
 */
static long
call(a, op, cmd, arg, len, cr, intr)
register struct sach *a;
long op, cmd, arg, len;
struct cred *cr;
int intr;
{
	int id, sig = 0;

	a->a_gaveup = 0;
	if (!sa_srv)
		return -EIO;
	if (++sa_seq == 0)
		sa_seq = 1;
	a->a_q.q_seq = sa_seq;
	a->a_q.q_ch = a < &sa_ch[SA_NSYN] ? a - sa_ch : SA_RESET;
	a->a_q.q_op = op;
	a->a_q.q_pid = u.u_procp->p_pid;
	a->a_q.q_uid = cr->cr_uid;
	a->a_q.q_cmd = cmd;
	a->a_q.q_arg = arg;
	a->a_q.q_len = len;
	a->a_state = POSTED;
	a->a_tmo = 0;
	wakeup((caddr_t)&sa_srv);
	pollwakeup(&sa_ph, POLLIN | POLLRDNORM);
	id = timeout(sa_tmo, (caddr_t)a, WAITTMO);
	while (a->a_state != ANSWERED) {
		/* the service is mid-copy in a_buf: wait it out first */
		if (!a->a_rd && (a->a_tmo || sig)) {
			untimeout(id);
			a->a_state = IDLE;
			a->a_gaveup = 1;
			return a->a_tmo ? -EIO : -EINTR;
		}
		if (sleep((caddr_t)a, intr && !sig ? SPACEPRI : WAITPRI))
			sig = 1;
	}
	untimeout(id);
	a->a_state = IDLE;
	return a->a_p.p_ret;
}

/*ARGSUSED*/
int
sa_open(devp, flag, otyp, cr)
dev_t *devp;
int flag, otyp;
struct cred *cr;
{
	register struct sach *a;
	long r;

	if (getminor(*devp) == SA_SRV) {
		if (drv_priv(cr))
			return EPERM;
		if (sa_srv)
			return EBUSY;
		sa_srv = 1;
		return 0;
	}
	if ((a = chan(*devp)) == 0 || !sa_srv)
		return ENXIO;
	if (a->a_open)
		return EBUSY;
	a->a_open = 1;
	acquire(a);
	r = call(a, (long)SAQ_OPEN, 0L, (long)flag, 0L, cr, 1);
	if (r < 0 && a->a_gaveup)	/* the service may have opened it */
		(void)call(a, (long)SAQ_CLOSE, 0L, 0L, 0L, cr, 0);
	release(a);
	if (r < 0) {
		a->a_open = 0;
		return errn(r);
	}
	return 0;
}

/*ARGSUSED*/
int
sa_close(dev, flag, otyp, cr)
dev_t dev;
int flag, otyp;
struct cred *cr;
{
	register struct sach *a;
	register int i;

	if (getminor(dev) == SA_SRV) {
		/* every caller fails; the service's restart sees no channel open */
		sa_srv = 0;
		for (i = 0; i < SA_NCH; i++) {
			a = &sa_ch[i];
			if (a->a_state == POSTED || a->a_state == TAKEN) {
				a->a_p.p_ret = -EIO;
				a->a_state = ANSWERED;
				wakeup((caddr_t)a);
			}
			a->a_space = 1;
			wakeup((caddr_t)&a->a_space);
		}
		return 0;
	}
	if ((a = chan(dev)) == 0)
		return ENXIO;
	acquire(a);
	(void)call(a, (long)SAQ_CLOSE, 0L, 0L, 0L, cr, 0);
	release(a);
	a->a_open = 0;
	return 0;
}

/*ARGSUSED*/
int
sa_read(dev, uio, cr)
dev_t dev;
struct uio *uio;
struct cred *cr;
{
	register struct sach *a;
	register int i;
	int e;

	if (getminor(dev) != SA_SRV)
		return ENXIO;
	for (;;) {
		for (i = 0; i < SA_NCH; i++)
			if (sa_ch[i].a_state == POSTED)
				break;
		if (i < SA_NCH)
			break;
		if (sleep((caddr_t)&sa_srv, (PZERO + 1) | PCATCH))
			return EINTR;
	}
	a = &sa_ch[i];
	if (uio->uio_resid < sizeof a->a_q + a->a_q.q_len) {
		/* it can never be read: fail the caller rather than spin the service */
		a->a_p.p_ret = -EIO;
		a->a_p.p_len = 0;
		a->a_state = ANSWERED;
		wakeup((caddr_t)a);
		return EINVAL;
	}
	a->a_rd = 1;
	e = uiomove((caddr_t)&a->a_q, sizeof a->a_q, UIO_READ, uio);
	if (e == 0)
		e = uiomove(a->a_buf, a->a_q.q_len, UIO_READ, uio);
	a->a_rd = 0;
	if (e == 0)
		a->a_state = TAKEN;
	wakeup((caddr_t)a);
	return e;
}

/*ARGSUSED*/
int
sa_write(dev, uio, cr)
dev_t dev;
struct uio *uio;
struct cred *cr;
{
	register struct sach *a;
	struct sarep p;
	long n, done, r, all;
	int e;

	if (getminor(dev) == SA_SRV) {
		if (uio->uio_resid < sizeof p)
			return EINVAL;
		if ((e = uiomove((caddr_t)&p, sizeof p, UIO_WRITE, uio)) != 0)
			return e;
		if (p.p_ch == SA_RESET)
			p.p_ch = SA_NSYN;
		if (p.p_ch < 0 || p.p_ch >= SA_NCH || p.p_len < 0 || p.p_len > SA_ARGMAX ||
		    p.p_len != uio->uio_resid)
			return EINVAL;
		a = &sa_ch[p.p_ch];
		if (p.p_seq == 0) {
			if (p.p_ret == SAP_SPACE) {
				a->a_space = 1;
				wakeup((caddr_t)&a->a_space);
			}
			return 0;
		}
		if (a->a_state != TAKEN || p.p_seq != a->a_q.q_seq)
			return ESRCH;
		a->a_rd = 1;
		e = uiomove(a->a_buf, p.p_len, UIO_WRITE, uio);
		a->a_rd = 0;
		if (e == 0) {
			a->a_p = p;
			a->a_state = ANSWERED;
		}
		wakeup((caddr_t)a);
		return e;
	}
	if ((a = chan(dev)) == 0)
		return ENXIO;
	all = uio->uio_resid;
	while (uio->uio_resid > 0) {
		n = uio->uio_resid < SA_MAX ? uio->uio_resid : SA_MAX;
		acquire(a);
		if ((e = uiomove(a->a_buf, n, UIO_WRITE, uio)) != 0) {
			release(a);
			return e;
		}
		/* what the service did not take moves to a_buf's start and waits for room */
		for (done = 0; done < n; done += r) {
			a->a_space = 0;
			if ((r = call(a, (long)SAQ_WRITE, 0L, 0L, n - done, cr, 1)) < 0) {
				release(a);
				uio->uio_resid += n - done;
				return uio->uio_resid == all ? errn(r) : 0;
			}
			if (r > n - done)
				r = n - done;
			if (done + r == n)
				continue;
			if (r)
				bcopy(a->a_buf + r, a->a_buf, (unsigned)(n - done - r));
			while (!a->a_space && sa_srv)
				if (sleep((caddr_t)&a->a_space, SPACEPRI)) {
					release(a);
					uio->uio_resid += n - done - r;
					return uio->uio_resid == all ? EINTR : 0;
				}
		}
		release(a);
	}
	return 0;
}

/*ARGSUSED*/
int
sa_ioctl(dev, cmd, arg, mode, cr, rvalp)
dev_t dev;
int cmd, arg, mode;
struct cred *cr;
int *rvalp;
{
	register struct sach *a;
	long len = 0, r;
	int e;

	if ((a = chan(dev)) == 0)
		return ENXIO;
	if (cmd & (IOCIN | IOCOUT)) {
		len = IOCSIZE(cmd);
		if (len > SA_ARGMAX)
			return EINVAL;
	}
	acquire(a);
	if (len && (cmd & IOCIN) && copyin((caddr_t)arg, a->a_buf, (unsigned)len)) {
		release(a);
		return EFAULT;
	}
	if (!(cmd & IOCIN))
		bzero(a->a_buf, (unsigned)len);
	r = call(a, (long)SAQ_IOCTL, (long)cmd, (long)arg, len, cr, 1);
	e = 0;
	if (r < 0)
		e = errn(r);
	else if ((cmd & IOCOUT) && a->a_p.p_len > 0 &&
	    copyout(a->a_buf, (caddr_t)arg, (unsigned)(a->a_p.p_len < len ? a->a_p.p_len : len)))
		e = EFAULT;
	release(a);
	*rvalp = (int)r;
	return e;
}

/*ARGSUSED*/
int
sa_poll(dev, events, anyyet, reventsp, phpp)
dev_t dev;
short events;
int anyyet;
short *reventsp;
struct pollhead **phpp;
{
	register int i;

	*reventsp = 0;
	if (getminor(dev) != SA_SRV)
		return ENXIO;
	for (i = 0; i < SA_NCH; i++)
		if (sa_ch[i].a_state == POSTED)
			*reventsp = events & (POLLIN | POLLRDNORM);
	if (*reventsp == 0 && !anyyet)
		*phpp = &sa_ph;
	return 0;
}
