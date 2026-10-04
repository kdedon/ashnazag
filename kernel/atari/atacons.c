/*
 * Console tty (major 0, minor 0): output to the frame-buffer console,
 * input from the IKBD key ring.  ldterm and ttcompat are pushed above.
 *
 * The keyboard interrupt only fills the ring and calls atacons_wake();
 * the read service builds the messages.  There is no line to pace, so
 * the termio settings are only kept for ldterm and the shell.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/dir.h"
#include "sys/file.h"
#include "sys/signal.h"
#include "sys/termio.h"
#include "sys/termios.h"
#include "sys/stream.h"
#include "sys/stropts.h"
#include "sys/strtty.h"
#include "sys/errno.h"
#include "sys/conf.h"
#include "sys/sysmacros.h"
#include "sys/systm.h"
#include "sys/cred.h"

#define CP_IN		1		/* cq_private: copyin done */
#define CP_OUT		2		/* cq_private: copyout done */
#define RXMSGSZ		64

int acopen(), acclose(), acwput(), acwsrv(), acrsrv();
void atacons_wake();
extern int ikbd_getc();
extern int ata_spltty();
extern void ata_splx();
extern void fbcons_write(), ata_nfwrite();

static struct module_info ac_minfo = {
	0x4143, "atacons", 0, INFPSZ, 512, 128,
};

static struct qinit ac_rinit = {
	NULL, acrsrv, acopen, acclose, NULL, &ac_minfo, NULL,
};

static struct qinit ac_winit = {
	acwput, acwsrv, NULL, NULL, NULL, &ac_minfo, NULL,
};

struct streamtab coinfo = {
	&ac_rinit, &ac_winit, NULL, NULL,
};

static struct strtty ac_tty;
static queue_t *ac_rq;
static int ac_bid;

static unsigned char ac_ccdef[NCCS] = {
	CINTR, CQUIT, CERASE, CKILL, CEOF, 0, 0, CSWTCH,
	CSTART, CSTOP, CSUSP, CDSUSP, CRPRNT, CFLUSH, CWERASE, CLNEXT,
};

static void ac_ioctl(), ac_iocdata(), ac_getattr(), ac_setattr();
static void ac_ack(), ac_nak(), ac_copyin(), ac_copyout(), ac_rflush();

/* ------------------------------------------------------ open, close */

int
acopen(rq, devp, flag, sflag, credp)
queue_t *rq;
dev_t *devp;
int flag, sflag;
cred_t *credp;
{
	register struct strtty *tp = &ac_tty;
	register mblk_t *mop;
	struct stroptions *sop;
	register int i;

	if (sflag)
		return EINVAL;
	if (getminor(*devp) != 0)
		return ENXIO;
	if (tp->t_state & ISOPEN)
		return 0;

	if ((mop = allocb(sizeof (struct stroptions), BPRI_MED)) == NULL)
		return EAGAIN;
	tp->t_state = CARR_ON;
	tp->t_dev = 0;
	tp->t_line = 0;
	tp->t_iflag = ICRNL | IXON | BRKINT | IGNPAR;
	tp->t_oflag = OPOST | ONLCR;
	tp->t_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
	tp->t_cflag = B9600 | CS8 | CREAD | CLOCAL;
	for (i = 0; i < NCCS; i++)
		tp->t_cc[i] = ac_ccdef[i];
	rq->q_ptr = WR(rq)->q_ptr = (caddr_t)tp;
	tp->t_rdqp = rq;
	tp->t_state |= ISOPEN;
	ac_rq = rq;

	mop->b_datap->db_type = M_SETOPTS;
	sop = (struct stroptions *)mop->b_wptr;
	mop->b_wptr += sizeof (struct stroptions);
	sop->so_flags = SO_HIWAT | SO_LOWAT | SO_ISTTY;
	sop->so_hiwat = 512;
	sop->so_lowat = 256;
	putnext(rq, mop);
	qenable(rq);		/* keys typed before the open */
	return 0;
}

int
acclose(q, flag, credp)
queue_t *q;
int flag;
cred_t *credp;
{
	if (ac_bid)
		unbufcall(ac_bid);
	ac_bid = 0;
	ac_rq = NULL;
	ac_tty.t_state = 0;
	ac_tty.t_rdqp = NULL;
	q->q_ptr = WR(q)->q_ptr = NULL;
	return 0;
}

/* ----------------------------------------------------------- read */

/* Keys arrived (any IPL). */
void
atacons_wake()
{
	if (ac_rq)
		qenable(ac_rq);
}

static void
ac_bufcb()
{
	ac_bid = 0;
	atacons_wake();
}

/* The key ring into M_DATA blocks as far as upstream flow control allows. */
int
acrsrv(q)
register queue_t *q;
{
	register mblk_t *mp;
	register int c;

	if (q->q_ptr == NULL)
		return 0;
	if (!(ac_tty.t_cflag & CREAD)) {
		ac_rflush();
		return 0;
	}
	for (;;) {
		if (!canput(q->q_next))
			return 0;
		if ((mp = allocb(RXMSGSZ, BPRI_HI)) == NULL) {
			if (!ac_bid)
				ac_bid = bufcall(RXMSGSZ, BPRI_HI, ac_bufcb, 0L);
			return 0;
		}
		while (mp->b_wptr < mp->b_datap->db_lim && (c = ikbd_getc()) >= 0)
			*mp->b_wptr++ = c;
		if (mp->b_wptr == mp->b_rptr) {
			freeb(mp);
			return 0;
		}
		putnext(q, mp);
	}
}

static void
ac_rflush()
{
	while (ikbd_getc() >= 0)
		;
}

/* ---------------------------------------------------------- write */

int
acwput(q, mp)
register queue_t *q;
register mblk_t *mp;
{
	register struct iocblk *iocp;

	switch (mp->b_datap->db_type) {
	case M_DATA:
		putq(q, mp);
		break;

	case M_IOCTL:
		iocp = (struct iocblk *)mp->b_rptr;
		switch (iocp->ioc_cmd) {
		case TCGETS: case TCGETA:
		case TCXONC: case TCFLSH:	/* must pass stopped output */
			ac_ioctl(q, mp);
			break;
		default:
			putq(q, mp);	/* in order with the data */
			break;
		}
		break;

	case M_IOCDATA:
		ac_iocdata(q, mp);
		break;

	case M_FLUSH:
		if (*mp->b_rptr & FLUSHW) {
			flushq(q, FLUSHDATA);
			*mp->b_rptr &= ~FLUSHW;
		}
		if (*mp->b_rptr & FLUSHR) {
			ac_rflush();
			flushq(RD(q), FLUSHDATA);
			qreply(q, mp);
		} else
			freemsg(mp);
		break;

	case M_STOP:
		ac_tty.t_state |= TTSTOP;
		freemsg(mp);
		break;

	case M_START:
		ac_tty.t_state &= ~TTSTOP;
		freemsg(mp);
		qenable(q);
		break;

	case M_CTL:
		iocp = (struct iocblk *)mp->b_rptr;
		if (mp->b_wptr - mp->b_rptr == sizeof (struct iocblk) &&
		    iocp->ioc_cmd == MC_CANONQUERY) {
			iocp->ioc_cmd = MC_DO_CANON;
			qreply(q, mp);
		} else
			freemsg(mp);
		break;

	default:
		freemsg(mp);
		break;
	}
	return 0;
}

int
acwsrv(q)
register queue_t *q;
{
	register mblk_t *mp, *bp;

	if (q->q_ptr == NULL)
		return 0;
	while ((mp = getq(q)) != NULL) {
		switch (mp->b_datap->db_type) {
		case M_DATA:
			if (ac_tty.t_state & TTSTOP) {
				putbq(q, mp);
				return 0;
			}
			for (bp = mp; bp; bp = bp->b_cont)
				if (bp->b_wptr > bp->b_rptr) {
					fbcons_write(bp->b_rptr,
					    (int)(bp->b_wptr - bp->b_rptr));
					ata_nfwrite(bp->b_rptr,
					    (int)(bp->b_wptr - bp->b_rptr));
				}
			freemsg(mp);
			break;
		case M_IOCTL:
			ac_ioctl(q, mp);
			break;
		default:
			freemsg(mp);
			break;
		}
	}
	return 0;
}

/* ---------------------------------------------------------- ioctls */

static void
ac_ack(q, mp, count)
queue_t *q;
register mblk_t *mp;
int count;
{
	register struct iocblk *iocp = (struct iocblk *)mp->b_rptr;

	mp->b_datap->db_type = M_IOCACK;
	iocp->ioc_count = count;
	iocp->ioc_error = 0;
	iocp->ioc_rval = 0;
	if (count == 0 && mp->b_cont) {
		freemsg(mp->b_cont);
		mp->b_cont = NULL;
	}
	qreply(q, mp);
}

static void
ac_nak(q, mp, err)
queue_t *q;
register mblk_t *mp;
int err;
{
	register struct iocblk *iocp = (struct iocblk *)mp->b_rptr;

	mp->b_datap->db_type = M_IOCNAK;
	iocp->ioc_count = 0;
	iocp->ioc_error = err;
	if (mp->b_cont) {
		freemsg(mp->b_cont);
		mp->b_cont = NULL;
	}
	qreply(q, mp);
}

/* Transparent ioctl: ask the stream head to copy in size bytes. */
static void
ac_copyin(q, mp, size)
queue_t *q;
register mblk_t *mp;
int size;
{
	register struct copyreq *cqp = (struct copyreq *)mp->b_rptr;
	caddr_t addr;

	if (mp->b_cont == NULL) {
		ac_nak(q, mp, EINVAL);
		return;
	}
	addr = *(caddr_t *)mp->b_cont->b_rptr;
	freemsg(mp->b_cont);
	mp->b_cont = NULL;
	cqp->cq_addr = addr;
	cqp->cq_size = size;
	cqp->cq_flag = 0;
	cqp->cq_private = (mblk_t *)CP_IN;
	mp->b_datap->db_type = M_COPYIN;
	mp->b_wptr = mp->b_rptr + sizeof (struct copyreq);
	qreply(q, mp);
}

/* Transparent ioctl: ask the stream head to copy out dp. */
static void
ac_copyout(q, mp, dp)
queue_t *q;
register mblk_t *mp, *dp;
{
	register struct copyreq *cqp = (struct copyreq *)mp->b_rptr;
	caddr_t addr;

	if (mp->b_cont == NULL) {
		freemsg(dp);
		ac_nak(q, mp, EINVAL);
		return;
	}
	addr = *(caddr_t *)mp->b_cont->b_rptr;
	freemsg(mp->b_cont);
	mp->b_cont = dp;
	cqp->cq_addr = addr;
	cqp->cq_size = dp->b_wptr - dp->b_rptr;
	cqp->cq_flag = 0;
	cqp->cq_private = (mblk_t *)CP_OUT;
	mp->b_datap->db_type = M_COPYOUT;
	mp->b_wptr = mp->b_rptr + sizeof (struct copyreq);
	qreply(q, mp);
}

static int
ac_setsize(cmd)
int cmd;
{
	switch (cmd) {
	case TCSETS: case TCSETSW: case TCSETSF:
		return sizeof (struct termios);
	case TCSETA: case TCSETAW: case TCSETAF:
		return sizeof (struct termio);
	}
	return 0;
}

/* Apply a termio or termios block. */
static void
ac_setattr(cmd, data)
int cmd;
caddr_t data;
{
	register struct strtty *tp = &ac_tty;
	register struct termio *to;
	register struct termios *ts;
	register int i;

	if (ac_setsize(cmd) == sizeof (struct termio)) {
		to = (struct termio *)data;
		tp->t_iflag = (tp->t_iflag & 0xFFFF0000) | to->c_iflag;
		tp->t_oflag = (tp->t_oflag & 0xFFFF0000) | to->c_oflag;
		tp->t_cflag = (tp->t_cflag & 0xFFFF0000) | to->c_cflag;
		tp->t_lflag = (tp->t_lflag & 0xFFFF0000) | to->c_lflag;
		for (i = 0; i < NCC; i++)
			tp->t_cc[i] = to->c_cc[i];
	} else {
		ts = (struct termios *)data;
		tp->t_iflag = ts->c_iflag;
		tp->t_oflag = ts->c_oflag;
		tp->t_cflag = ts->c_cflag;
		tp->t_lflag = ts->c_lflag;
		for (i = 0; i < NCCS; i++)
			tp->t_cc[i] = ts->c_cc[i];
	}
	if (cmd == TCSETAF || cmd == TCSETSF)
		ac_rflush();
}

static void
ac_getattr(q, mp)
queue_t *q;
register mblk_t *mp;
{
	register struct strtty *tp = &ac_tty;
	register struct iocblk *iocp = (struct iocblk *)mp->b_rptr;
	register mblk_t *dp;
	register struct termios *ts;
	register struct termio *to;
	register int i, size;

	size = iocp->ioc_cmd == TCGETS ?
	    sizeof (struct termios) : sizeof (struct termio);
	if ((dp = allocb(size, BPRI_MED)) == NULL) {
		ac_nak(q, mp, EAGAIN);
		return;
	}
	if (iocp->ioc_cmd == TCGETS) {
		ts = (struct termios *)dp->b_wptr;
		ts->c_iflag = tp->t_iflag;
		ts->c_oflag = tp->t_oflag;
		ts->c_cflag = tp->t_cflag;
		ts->c_lflag = tp->t_lflag;
		for (i = 0; i < NCCS; i++)
			ts->c_cc[i] = tp->t_cc[i];
	} else {
		to = (struct termio *)dp->b_wptr;
		to->c_iflag = tp->t_iflag;
		to->c_oflag = tp->t_oflag;
		to->c_cflag = tp->t_cflag;
		to->c_lflag = tp->t_lflag;
		to->c_line = tp->t_line;
		for (i = 0; i < NCC; i++)
			to->c_cc[i] = tp->t_cc[i];
	}
	dp->b_wptr += size;
	if (iocp->ioc_count == TRANSPARENT) {
		ac_copyout(q, mp, dp);
		return;
	}
	if (mp->b_cont)
		freemsg(mp->b_cont);
	mp->b_cont = dp;
	ac_ack(q, mp, size);
}

static void
ac_ioctl(q, mp)
queue_t *q;
register mblk_t *mp;
{
	register struct iocblk *iocp = (struct iocblk *)mp->b_rptr;
	register int arg, size;

	if ((size = ac_setsize(iocp->ioc_cmd)) != 0) {
		if (iocp->ioc_count == TRANSPARENT) {
			ac_copyin(q, mp, size);
			return;
		}
		if (mp->b_cont == NULL ||
		    mp->b_cont->b_wptr - mp->b_cont->b_rptr < size) {
			ac_nak(q, mp, EINVAL);
			return;
		}
		ac_setattr(iocp->ioc_cmd, (caddr_t)mp->b_cont->b_rptr);
		ac_ack(q, mp, 0);
		return;
	}
	if (iocp->ioc_cmd == TCGETS || iocp->ioc_cmd == TCGETA) {
		ac_getattr(q, mp);
		return;
	}

	/* the rest take an int argument by value */
	if (mp->b_cont == NULL ||
	    mp->b_cont->b_wptr - mp->b_cont->b_rptr < sizeof (int)) {
		if ((iocp->ioc_cmd & IOCTYPE) == LDIOC)
			ac_ack(q, mp, 0);
		else
			ac_nak(q, mp, EINVAL);
		return;
	}
	arg = *(int *)mp->b_cont->b_rptr;

	switch (iocp->ioc_cmd) {
	case TCSBRK:
		ac_ack(q, mp, 0);
		return;

	case TCXONC:
		switch (arg) {
		case TCOOFF:
			ac_tty.t_state |= TTSTOP;
			break;
		case TCOON:
			ac_tty.t_state &= ~TTSTOP;
			qenable(q);
			break;
		case TCIOFF:
		case TCION:
			break;
		default:
			ac_nak(q, mp, EINVAL);
			return;
		}
		ac_ack(q, mp, 0);
		return;

	case TCFLSH:
		if (arg < TCIFLUSH || arg > TCIOFLUSH) {
			ac_nak(q, mp, EINVAL);
			return;
		}
		if (arg != TCOFLUSH)
			ac_rflush();
		if (arg != TCIFLUSH)
			flushq(q, FLUSHDATA);
		ac_ack(q, mp, 0);
		return;
	}

	if ((iocp->ioc_cmd & IOCTYPE) == LDIOC)
		ac_ack(q, mp, 0);
	else
		ac_nak(q, mp, EINVAL);
}

/* Reply to our M_COPYIN / M_COPYOUT. */
static void
ac_iocdata(q, mp)
queue_t *q;
register mblk_t *mp;
{
	register struct copyresp *csp = (struct copyresp *)mp->b_rptr;
	register mblk_t *dp = mp->b_cont;
	register int size;

	if (csp->cp_rval) {
		freemsg(mp);
		return;
	}
	if (csp->cp_private == (mblk_t *)CP_OUT) {
		ac_ack(q, mp, 0);
		return;
	}
	size = ac_setsize(csp->cp_cmd);
	if (size == 0 || dp == NULL || dp->b_wptr - dp->b_rptr < size) {
		ac_nak(q, mp, EINVAL);
		return;
	}
	ac_setattr(csp->cp_cmd, (caddr_t)dp->b_rptr);
	ac_ack(q, mp, 0);
}
