/*
 * otbdev.c -- /dev/otbridge sessions, the event ring, and the module's
 * linkages.
 *
 * Every open of minor 0 clones a session owned by the opening process.
 * The session's ring lives in that process: OTB_SETRING soft-locks its
 * pages and records their physical addresses, and the kernel writes
 * entries through the supervisor identity window (physical = virtual
 * below 1 GB), from any context, but only while the owner still maps
 * those pages there.  A fork followed by a copy-on-write, an exec, an
 * unmap or the owner's exit loses the ring: posts stop and the owner
 * gets the signal; OTB_GETRING tells, OTB_SETRING registers again.
 *
 * K&R C.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/immu.h"
#include "sys/signal.h"
#include "sys/stream.h"
#include "sys/errno.h"
#include "sys/cred.h"
#include "sys/proc.h"
#include "sys/kmem.h"
#include "sys/conf.h"
#include "sys/uio.h"
#include "sys/poll.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "sys/moddefs.h"
#include "otbridge.h"

extern struct proc *curproc, *prfind();
extern int copyin(), copyout(), nodev();
extern void bzero();
extern unsigned long vtop();
extern faultcode_t as_fault();
extern char *kmem_zalloc();
extern void kmem_free();
extern struct streamtab otxtiinfo;

#define	PGSZ		2048		/* smallest page the port maps */
#define	TTLIMIT		0x40000000	/* end of the identity window */
#define	SIGPOLL_	22
#define	OTB_USESS	8		/* sessions per user but root */
#define	OTB_MAXLEAK	64		/* rings left locked before SETRING refuses */

struct otbs	otb_sess[OTB_NSESS];
struct otx	*otb_eps;
struct otb_stats otb_st;
int		otb_incall;
int		otb_devflag = 0;

typedef unsigned char __volatile__ vu_char;

char *
otb_alloc(n)
	int n;
{
	int s;

	OTB_SPLSTR(s);
	otb_st.st_kmem += n;
	OTB_SPLX(s);
	return kmem_zalloc((size_t)n, KM_SLEEP);
}

void
otb_free(p, n)
	char *p;
	int n;
{
	int s;

	OTB_SPLSTR(s);
	otb_st.st_kmem -= n;
	OTB_SPLX(s);
	kmem_free(p, (size_t)n);
}

struct otbs *
otb_sfind(id)
	long id;
{
	if (id < 2 || id >= OTB_NSESS + 2 || !otb_sess[id - 2].s_inuse)
		return 0;
	return &otb_sess[id - 2];
}

/* at splstr */
void
otb_sdetach(x)
	struct otx *x;
{
	struct otx **pp;

	if (x->x_sess == 0)
		return;
	for (pp = &x->x_sess->s_eps; *pp; pp = &(*pp)->x_snext)
		if (*pp == x) {
			*pp = x->x_snext;
			break;
		}
	x->x_sess = 0;
	x->x_snext = 0;
	x->x_ringidx = -1;
}

/* ---- ring ---- */

static vu_char *
rp(s, off)
	struct otbs *s;
	int off;
{
	unsigned long a = s->s_uva + off;

	return (vu_char *)(s->s_pa[a / PGSZ - s->s_uva / PGSZ] + a % PGSZ);
}

#define	RG16(s, o)	(rp(s, o)[0] << 8 | rp(s, o)[1])
#define	RP16(s, o, v)	(rp(s, o)[0] = ((v) >> 8) & 0xff, rp(s, o)[1] = (v) & 0xff)

static void
rp32(s, off, v)
	struct otbs *s;
	int off;
	unsigned long v;
{
	RP16(s, off, v >> 16);
	RP16(s, off + 2, v & 0xffff);
}

static unsigned long
rg32(s, off)
	struct otbs *s;
	int off;
{
	return (unsigned long)RG16(s, off) << 16 | RG16(s, off + 2);
}

/* the owner, with the address space that locked the ring, or 0 */
static struct proc *
owner(s)
	struct otbs *s;
{
	struct proc *p = prfind((pid_t)s->s_pid);

	return p && p == s->s_proc && p->p_as == s->s_as ? p : 0;
}

/* the owner still maps the locked pages at the ring's address */
static int
mapped(s)
	struct otbs *s;
{
	struct proc *p = owner(s);
	unsigned long a;
	int k;

	if (p == 0 || s->s_npg == 0)
		return 0;
	for (k = 0; k < s->s_npg; k++) {
		a = k ? (s->s_uva / PGSZ + k) * PGSZ : s->s_uva;
		if (vtop((caddr_t)a, p) - (k ? 0 : s->s_uva % PGSZ) != s->s_pa[k])
			return 0;
	}
	return 1;
}

/* at splstr: stop posting; the pages stay locked until ringoff */
static void
ringlost(s)
	struct otbs *s;
{
	struct otx *x;

	s->s_ringon = 0;
	for (x = s->s_eps; x; x = x->x_snext)
		x->x_ringidx = -1;
	otb_st.st_ringlost++;
	if (owner(s))
		psignal(s->s_proc, s->s_sig);
}

#define	ENT(i)		(OTB_RING_HDR + (i) * OTB_RING_ENT)

void
otb_post(x, ev)
	struct otx *x;
	int ev;
{
	struct otbs *s;
	int sp, head, tail, n, i;

	OTB_SPLSTR(sp);
	if ((s = x->x_sess) == 0 || !s->s_ringon)
		goto out;
	if (!mapped(s)) {
		ringlost(s);
		goto out;
	}
	head = s->s_head;
	tail = RG16(s, 8);
	if (tail >= OTB_RING_N)
		tail = head;
	n = (head - tail + OTB_RING_N) % OTB_RING_N;
	i = x->x_ringidx;
	if (i >= 0 && (i - tail + OTB_RING_N) % OTB_RING_N < n &&
	    rg32(s, ENT(i)) == x->x_cookie && RG16(s, ENT(i) + 4) != 0) {
		RP16(s, ENT(i) + 4, RG16(s, ENT(i) + 4) | ev);
		otb_st.st_merged++;
		goto out;
	}
	if (n == OTB_RING_N - 1) {
		RP16(s, 10, RG16(s, 10) | OTB_RING_OVERFLOW);
		x->x_ringidx = -1;
		otb_st.st_overflows++;
		goto out;
	}
	rp32(s, ENT(head), x->x_cookie);
	RP16(s, ENT(head) + 4, ev);
	RP16(s, ENT(head) + 6, 0);
	x->x_ringidx = head;
	s->s_head = (head + 1) % OTB_RING_N;
	RP16(s, 6, s->s_head);
	otb_st.st_posts++;
	if (n == 0) {
		psignal(s->s_proc, s->s_sig);
		otb_st.st_signals++;
	}
out:
	OTB_SPLX(sp);
}

/*
 * Unlock the ring's pages.  Pages the owner no longer maps (after a
 * fork and copy-on-write, or the owner's exit) cannot be unlocked
 * through its address space and stay locked.
 */
static void
ringoff(s)
	struct otbs *s;
{
	struct otx *x;
	int sp, ok;

	if (s->s_npg == 0)
		return;
	OTB_SPLSTR(sp);
	ok = mapped(s);
	s->s_ringon = 0;
	for (x = s->s_eps; x; x = x->x_snext)
		x->x_ringidx = -1;
	OTB_SPLX(sp);
	if (ok)
		(void)as_fault(s->s_as, (caddr_t)s->s_uva, (u_int)OTB_RINGSZ,
		    F_SOFTUNLOCK, S_WRITE);
	else
		otb_st.st_ringleak++;
	s->s_npg = 0;
}

static int
ringon(s, sr)
	struct otbs *s;
	struct otb_setring *sr;
{
	unsigned long a = sr->sr_addr, pa;
	int k, sig = sr->sr_sig ? sr->sr_sig : SIGPOLL_;

	if ((a & 7) || sig <= 0 || sig >= NSIG)
		return EINVAL;
	if (otb_st.st_ringleak >= OTB_MAXLEAK)
		return EAGAIN;
	if (as_fault(curproc->p_as, (caddr_t)a, (u_int)OTB_RINGSZ, F_SOFTLOCK,
	    S_WRITE) != 0)
		return EFAULT;
	for (k = 0; k <= (a + OTB_RINGSZ - 1) / PGSZ - a / PGSZ; k++) {
		pa = vtop((caddr_t)(k ? (a / PGSZ + k) * PGSZ : a), curproc);
		pa -= (k ? 0 : a % PGSZ);
		if (pa == 0 || pa >= TTLIMIT) {
			(void)as_fault(curproc->p_as, (caddr_t)a, (u_int)OTB_RINGSZ,
			    F_SOFTUNLOCK, S_WRITE);
			return EINVAL;
		}
		s->s_pa[k] = pa;
	}
	s->s_npg = k;
	s->s_as = curproc->p_as;
	s->s_uva = a;
	s->s_sig = sig;
	s->s_head = 0;
	rp32(s, 0, (unsigned long)OTB_RMAGIC);
	RP16(s, 4, OTB_RING_N);
	RP16(s, 6, 0);
	RP16(s, 8, 0);
	RP16(s, 10, 0);
	rp32(s, 12, 0L);
	s->s_ringon = 1;
	return 0;
}

/* ---- device ---- */

/*ARGSUSED*/
int
otbopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	cred_t *cr;
{
	struct otbs *s;
	int i, n, sp;

	if (getminor(*devp) == 1)
		return otbst_open(devp, cr);
	if (getminor(*devp) != 0)
		return ENXIO;
	otb_incall++;
	OTB_SPLSTR(sp);
	for (i = n = 0; i < OTB_NSESS; i++)
		if (otb_sess[i].s_inuse && otb_sess[i].s_uid == cr->cr_uid)
			n++;
	for (i = 0; i < OTB_NSESS && otb_sess[i].s_inuse; i++)
		;
	if (i == OTB_NSESS || (cr->cr_uid != 0 && n >= OTB_USESS)) {
		OTB_SPLX(sp);
		otb_incall--;
		return EAGAIN;
	}
	s = &otb_sess[i];
	bzero((caddr_t)s, sizeof *s);
	s->s_inuse = 1;
	s->s_id = i + 2;
	s->s_proc = curproc;
	s->s_pid = curproc->p_pid;
	s->s_uid = cr->cr_uid;
	otb_st.st_sessions++;
	OTB_SPLX(sp);
	*devp = makedevice(getmajor(*devp), i + 2);
	otb_incall--;
	return 0;
}

/*ARGSUSED*/
int
otbclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	cred_t *cr;
{
	struct otbs *s = otb_sfind((long)getminor(dev));
	int sp;

	if (getminor(dev) >= OTB_STMINOR)
		return otbst_close(dev);
	if (s == 0)
		return 0;
	otb_incall++;
	ringoff(s);
	OTB_SPLSTR(sp);
	while (s->s_eps)
		otb_sdetach(s->s_eps);
	s->s_inuse = 0;
	otb_st.st_sessions--;
	OTB_SPLX(sp);
	otb_incall--;
	return 0;
}

/*ARGSUSED*/
int
otbioctl(dev, cmd, arg, mode, cr, rvalp)
	dev_t dev;
	int cmd, mode;
	caddr_t arg;
	cred_t *cr;
	int *rvalp;
{
	struct otbs *s = otb_sfind((long)getminor(dev));
	struct otb_setring sr;
	int e = 0;

	if (getminor(dev) >= OTB_STMINOR)
		return otbst_ioctl(dev, cmd, arg, cr, rvalp);
	if (s == 0)
		return ENXIO;
	otb_incall++;
	switch (cmd) {
	case OTB_GETID:
		*rvalp = s->s_id;
		break;
	case OTB_GETRING:
		*rvalp = s->s_ringon;
		break;
	case OTB_SETRING:
		if (s->s_proc != curproc || s->s_pid != curproc->p_pid)
			e = EPERM;
		else if (copyin(arg, (caddr_t)&sr, sizeof sr))
			e = EFAULT;
		else {
			ringoff(s);
			if (sr.sr_addr)
				e = ringon(s, &sr);
		}
		break;
	case OTB_STATS:
		if (copyout((caddr_t)&otb_st, arg, sizeof otb_st))
			e = EFAULT;
		break;
	default:
		e = EINVAL;
	}
	otb_incall--;
	return e;
}

/*ARGSUSED*/
int
otbread(dev, uiop, cr)
	dev_t dev;
	struct uio *uiop;
	cred_t *cr;
{
	return getminor(dev) >= OTB_STMINOR ? otbst_read(dev, uiop) : ENXIO;
}

/*ARGSUSED*/
int
otbwrite(dev, uiop, cr)
	dev_t dev;
	struct uio *uiop;
	cred_t *cr;
{
	return getminor(dev) >= OTB_STMINOR ? otbst_write(dev, uiop) : ENXIO;
}

int
otbchpoll(dev, events, anyyet, reventsp, phpp)
	dev_t dev;
	short events;
	int anyyet;
	short *reventsp;
	struct pollhead **phpp;
{
	if (getminor(dev) < OTB_STMINOR)
		return ENXIO;
	return otbst_poll(dev, events, anyyet, reventsp, phpp);
}

/* ---- linkages ---- */

static int
otb_load()
{
	return 0;
}

static int
otb_unload()
{
	return otb_st.st_sessions || otb_st.st_endpoints || otb_nst || otb_incall ?
	    EBUSY : 0;
}

struct mod_str_data otbridge_strdata[] = { { "otxti", &otxtiinfo, &otb_devflag } };

/* major OTB_MAJOR, registered with modadm(MOD_TY_CDEV) before loading */
struct mod_drv_data otbridge_drvdata[] = { {
	{ 0 }, 0, 0,
	{ otbopen, otbclose, otbread, otbwrite, otbioctl, nodev, nodev,
	  otbchpoll, nodev, nodev, 0, 0, &otb_devflag },
	OTB_MAJOR, 1,
} };

static struct mod_type_data otb_std = { "otxti module", (char *)otbridge_strdata };
static struct mod_type_data otb_dtd = { "otbridge sessions", (char *)otbridge_drvdata };

static struct modlink otb_links[] = {
	{ &mod_strops, (char *)&otb_std },
	{ &mod_drvops, (char *)&otb_dtd },
	{ 0, 0 }
};

asm(".weak otbridge_conf_data");
extern struct mod_conf_data otbridge_conf_data;

struct modwrapper otbridge_wrapper = {
	MODREV, otb_load, otb_unload, 0, &otbridge_conf_data, otb_links
};
