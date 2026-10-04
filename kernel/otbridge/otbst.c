/*
 * otbst.c -- /dev/otbridge minor 1: virtual Ethernet stations.  Each open
 * clones a station; OTB_STATION gives it an address on the host's NIC
 * (vstation.h).  Received frames queue here until read; writes go out
 * with the station's source address only.
 *
 * Policy: root may choose any unicast address.  Other users need group
 * otb_stgid (the display group, as for the Mac session's screen) or
 * otb_stuser set; they get one station each, with the derived address
 * (locally administered: 02, the host address's last three bytes, the
 * uid).
 *
 * K&R C.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/signal.h"
#include "sys/stream.h"
#include "sys/errno.h"
#include "sys/cred.h"
#include "sys/proc.h"
#include "sys/uio.h"
#include "sys/file.h"
#include "sys/poll.h"
#include "otbridge.h"
#include "vstation.h"

asm(".weak vst_ops");

#define	OTB_NST		4
#define	OTB_STQ		8		/* frames queued per station */
#define	OTB_STWAIT	10		/* ticks a blocking write waits for room */

struct otbst {
	int		t_inuse;
	int		t_vst;		/* NIC station, -1 before OTB_STATION */
	struct proc	*t_proc;
	pid_t		t_pid;
	uid_t		t_uid;
	int		t_sig;
	int		t_n;
	mblk_t		*t_head, *t_tail;
	unsigned char	t_mac[6];
	struct pollhead	t_ph;
	struct otb_ststats t_st;
};

struct otbst	otb_st_[OTB_NST];
int		otb_stuser = 0;		/* any user may have a station */
int		otb_stgid = 25;		/* users of this group may */
int		otb_nst;		/* open stations */

extern mblk_t *allocb();
extern struct proc *curproc;
extern int copyin(), copyout(), bcmp(), sleep(), uiomove(), groupmember();
extern void delay();
extern void bcopy(), bzero(), wakeup(), psignal(), pollwakeup(), freemsg();

static struct otbst *
stfind(dev)
	dev_t dev;
{
	int i = getminor(dev) - OTB_STMINOR;

	return i >= 0 && i < OTB_NST && otb_st_[i].t_inuse ? &otb_st_[i] : 0;
}

/* NIC receive, at its interrupt level */
static void
strx(arg, pkt, len)
	char *arg;
	unsigned char *pkt;
	int len;
{
	struct otbst *t = (struct otbst *)arg;
	mblk_t *mp;
	int sp;

	OTB_SPLSTR(sp);
	if (t->t_n >= OTB_STQ || (mp = allocb(len, BPRI_MED)) == 0) {
		t->t_st.ss_drop++;
		OTB_SPLX(sp);
		return;
	}
	bcopy((caddr_t)pkt, (caddr_t)mp->b_wptr, len);
	mp->b_wptr += len;
	if (t->t_tail)
		t->t_tail->b_next = mp;
	else
		t->t_head = mp;
	t->t_tail = mp;
	t->t_st.ss_rx++;
	if (t->t_n++ == 0) {
		wakeup((caddr_t)t);
		pollwakeup(&t->t_ph, POLLIN | POLLRDNORM);
		if (t->t_sig && t->t_proc->p_pid == t->t_pid)
			psignal(t->t_proc, t->t_sig);
	}
	OTB_SPLX(sp);
}

int
otbst_open(devp, cr)
	dev_t *devp;
	cred_t *cr;
{
	struct otbst *t;
	int i, sp;

	if (cr->cr_uid != 0 && !otb_stuser && !groupmember(otb_stgid, cr))
		return EPERM;
	if (&vst_ops == 0 || vst_ops == 0)
		return ENXIO;
	OTB_SPLSTR(sp);
	for (i = 0; cr->cr_uid != 0 && i < OTB_NST; i++)
		if (otb_st_[i].t_inuse && otb_st_[i].t_uid == cr->cr_uid) {
			OTB_SPLX(sp);
			return EBUSY;
		}
	for (i = 0; i < OTB_NST && otb_st_[i].t_inuse; i++)
		;
	if (i == OTB_NST) {
		OTB_SPLX(sp);
		return EAGAIN;
	}
	t = &otb_st_[i];
	bzero((caddr_t)t, sizeof *t);
	t->t_inuse = 1;
	t->t_vst = -1;
	t->t_proc = curproc;
	t->t_pid = curproc->p_pid;
	t->t_uid = cr->cr_uid;
	otb_nst++;
	OTB_SPLX(sp);
	*devp = makedevice(getmajor(*devp), OTB_STMINOR + i);
	return 0;
}

int
otbst_close(dev)
	dev_t dev;
{
	struct otbst *t = stfind(dev);
	mblk_t *mp;
	int sp;

	if (t == 0)
		return 0;
	if (t->t_vst >= 0)
		(*vst_ops->vs_detach)(t->t_vst);
	OTB_SPLSTR(sp);
	while ((mp = t->t_head) != 0) {
		t->t_head = mp->b_next;
		mp->b_next = 0;
		freemsg(mp);
	}
	t->t_inuse = 0;
	otb_nst--;
	OTB_SPLX(sp);
	return 0;
}

int
otbst_read(dev, uiop)
	dev_t dev;
	struct uio *uiop;
{
	struct otbst *t = stfind(dev);
	mblk_t *mp;
	int sp, n, e;

	if (t == 0)
		return ENXIO;
	OTB_SPLSTR(sp);
	while ((mp = t->t_head) == 0) {
		if (t->t_vst < 0 || uiop->uio_fmode & (FNDELAY | FNONBLOCK)) {
			OTB_SPLX(sp);
			return uiop->uio_fmode & FNONBLOCK ? EAGAIN : 0;
		}
		if (sleep((caddr_t)t, (PZERO + 1) | PCATCH)) {
			OTB_SPLX(sp);
			return EINTR;
		}
	}
	if ((t->t_head = mp->b_next) == 0)
		t->t_tail = 0;
	mp->b_next = 0;
	t->t_n--;
	OTB_SPLX(sp);
	n = mp->b_wptr - mp->b_rptr;
	if (n > uiop->uio_resid)
		n = uiop->uio_resid;
	e = uiomove((caddr_t)mp->b_rptr, (long)n, UIO_READ, uiop);
	freemsg(mp);
	return e;
}

int
otbst_write(dev, uiop)
	dev_t dev;
	struct uio *uiop;
{
	struct otbst *t = stfind(dev);
	int n = uiop->uio_resid, e, w;
	mblk_t *mp;

	if (t == 0)
		return ENXIO;
	if (t->t_vst < 0)
		return EINVAL;
	if (n < VST_MINFRAME || n > VST_MAXFRAME)
		return EINVAL;
	if ((mp = allocb(n, BPRI_MED)) == 0)
		return ENOSR;
	/* a blocking write waits a few ticks for transmit room */
	if ((e = uiomove((caddr_t)mp->b_rptr, (long)n, UIO_WRITE, uiop)) == 0)
		for (w = 0; (e = (*vst_ops->vs_xmit)(t->t_vst, mp->b_rptr, n)) == EAGAIN &&
		    !(uiop->uio_fmode & (FNDELAY | FNONBLOCK)) && w < OTB_STWAIT; w++)
			delay(1);
	if (e == 0)
		t->t_st.ss_tx++;
	freemsg(mp);
	return e;
}

static int
attach(t, st, cr)
	struct otbst *t;
	struct otb_station *st;
	cred_t *cr;
{
	unsigned char h[6];
	int v;

	if (t->t_vst >= 0)
		return EBUSY;
	if (st->st_mode != OTB_ST_BRIDGE)
		return EINVAL;
	if (bcmp((caddr_t)st->st_mac, "\0\0\0\0\0\0", 6) == 0) {
		(*vst_ops->vs_hwaddr)(h);
		st->st_mac[0] = 0x02;
		st->st_mac[1] = h[3];
		st->st_mac[2] = h[4];
		st->st_mac[3] = h[5];
		st->st_mac[4] = t->t_uid >> 8;
		st->st_mac[5] = t->t_uid;
	} else if (cr->cr_uid != 0)
		return EPERM;
	if ((v = (*vst_ops->vs_attach)(st->st_mac, strx, (char *)t)) < 0)
		return -v;
	bcopy((caddr_t)st->st_mac, (caddr_t)t->t_mac, 6);
	t->t_vst = v;
	return 0;
}

int
otbst_ioctl(dev, cmd, arg, cr, rvalp)
	dev_t dev;
	int cmd;
	caddr_t arg;
	cred_t *cr;
	int *rvalp;
{
	struct otbst *t = stfind(dev);
	struct otb_station st;
	struct otb_stmulti sm;
	struct otb_ststats ss;
	int e = 0;

	if (t == 0)
		return ENXIO;
	*rvalp = 0;
	switch (cmd) {
	case OTB_STATION:
		if (copyin(arg, (caddr_t)&st, sizeof st))
			return EFAULT;
		if ((e = attach(t, &st, cr)) == 0 &&
		    copyout((caddr_t)&st, arg, sizeof st))
			e = EFAULT;
		return e;
	case OTB_STSIG:
		if ((int)arg < 0 || (int)arg >= NSIG)
			return EINVAL;
		t->t_sig = (int)arg;
		t->t_proc = curproc;
		t->t_pid = curproc->p_pid;
		return 0;
	case OTB_STMULTI:
		if (t->t_vst < 0)
			return EINVAL;
		if (copyin(arg, (caddr_t)&sm, sizeof sm))
			return EFAULT;
		return (*vst_ops->vs_mcast)(t->t_vst, sm.sm_addr, sm.sm_on != 0);
	case OTB_STSTATS:
		ss = t->t_st;
		ss.ss_queued = t->t_n;
		return copyout((caddr_t)&ss, arg, sizeof ss) ? EFAULT : 0;
	}
	return EINVAL;
}

int
otbst_poll(dev, events, anyyet, reventsp, phpp)
	dev_t dev;
	short events;
	int anyyet;
	short *reventsp;
	struct pollhead **phpp;
{
	struct otbst *t = stfind(dev);
	short r = 0;

	if (t == 0)
		return ENXIO;
	if (events & (POLLIN | POLLRDNORM) && t->t_head)
		r |= events & (POLLIN | POLLRDNORM);
	if (t->t_vst >= 0 && (*vst_ops->vs_room)())
		r |= events & POLLOUT;
	*reventsp = r;
	if (r == 0 && !anyyet)
		*phpp = &t->t_ph;
	return 0;
}
