/*
 * genv.c -- environment sessions: claim and exit, the device node, the
 * clock tick, virtual interrupts and their carrier signal, stop, and
 * the host-call gate.
 *
 * K&R C.
 */

#include "kinc.h"
#include "sys/sysm68k.h"
#include "guest_rpage.h"
#include "guest_env.h"

extern int ttimeout(), untimeout();

struct guest_sess *
guest_env_sess(gp)
	struct guest_proc *gp;
{
	struct guest_env *e = GENV(gp);
	int i;

	for (i = 0; i < e->ge_nsess; i++)
		if (e->ge_sess[i].gs_state != GS_FREE && e->ge_sess[i].gs_gp == gp)
			return &e->ge_sess[i];
	return 0;
}

int
guest_env_level(s)
	struct guest_sess *s;
{
	int l;

	if (s->gs_env->ge_level)
		return (*s->gs_env->ge_level)(s);
	for (l = 7; l > 0; l--)
		if (s->gs_pend & 1 << l)
			return l;
	return 0;
}

void
guest_env_note(s, what, a, pc)
	struct guest_sess *s;
	char *what;
	long a, pc;
{
	if (s->gs_env->ge_note)
		(*s->gs_env->ge_note)(s, what, a, pc);
	else
		printf("NOTICE: %s: %s %x pc %x\n", s->gs_env->ge_name, what, (int)a, (int)pc);
}

/* a sleeping guest is woken; a running one gets the carrier if the IPL lets the request in */
void
guest_env_kick(s)
	struct guest_sess *s;
{
	int x = guest_splhi(), l;

	if (s->gs_state == GS_RUN) {
		l = guest_env_level(s);
		s->gs_gp->gp_vpend = l << 8;
		if (s->gs_sleeping)
			wakeup((caddr_t)&s->gs_sleeping);
		else if (l > GUEST_VIPL(s->gs_gp))
			psignal(s->gs_proc, s->gs_env->ge_sig);
	}
	guest_splx(x);
}

void
guest_env_post(s, l)
	struct guest_sess *s;
	int l;
{
	int x = guest_splhi();

	s->gs_pend |= 1 << l;
	guest_env_kick(s);
	guest_splx(x);
}

static int
genv_ack(s, r, l)
	struct guest_sess *s;
	char *r;
	int l;
{
	if (s->gs_env->ge_ack)
		return (*s->gs_env->ge_ack)(s, r, l);
	s->gs_pend &= ~(1UL << l);
	return 24 + l;
}

/* every interrupt the IPL lets through, each frame on top of the last */
void
guest_env_intr(gp, r)
	struct guest_proc *gp;
	char *r;
{
	struct guest_sess *s = guest_env_sess(gp);
	struct guest_env *e = GENV(gp);
	int x, l, v;

	if (!s || s->gs_state != GS_RUN)
		return;
	/* a fault frame resumes part way through its instruction */
	if ((e->ge_holdfmt >> (GR_FV(r) >> 12) & 1) && !(u.u_sigflag & USTKCLEAR))
		return;
	if (e->ge_sync && (*e->ge_sync)(s))
		return;
	x = guest_splhi();
	while ((l = guest_env_level(s)) > GUEST_VIPL(gp)) {
		guest_splx(x);
		/* the frame goes to guest memory, which may fault */
		if ((e->ge_flags & GEF_SIGTAIL) && !guest_sigtail(r)) {
			x = guest_splhi();
			break;
		}
		if (s->gs_sinkva || s->gs_sinktrace)
			guest_cpage_unsink(s, r);
		x = guest_splhi();
		v = genv_ack(s, r, l);
		guest_splx(x);
		if (v >= 0) {
			if (guest_reflect(gp, r, GR_PC(r), v << 2, (char *)0, 2, l) < 0) {
				guest_env_note(s, "interrupt without a stack", (long)l, GR_PC(r));
				psignal(curproc, SIGSEGV);
				return;
			}
			s->gs_ints++;
			if (GR_FV(r) >> 12)
				u.u_sigflag |= USTKCLEAR;
		}
		x = guest_splhi();
		if (e->ge_flags & GEF_ONE)
			break;
	}
	l = guest_env_level(s);
	gp->gp_vpend = l << 8;
	if (l <= GUEST_VIPL(gp))
		sigdelset(&gp->gp_proc->p_sig, e->ge_sig);
	guest_splx(x);
}

/* the carrier waits while the guest's IPL masks every request */
static int
genv_fsig(p, gp)
	struct proc *p;
	struct guest_proc *gp;
{
	struct guest_sess *s = guest_env_sess(gp);
	struct guest_env *e = GENV(gp);
	k_sigset_t h;
	int x, n, held;

	held = s && e->ge_sync && (*e->ge_sync)(s);
	x = guest_splhi();
	h = p->p_hold;
	sigdelset(&p->p_hold, e->ge_sig);
	if (!s || held || guest_env_level(s) <= GUEST_VIPL(gp) ||
	    ((e->ge_flags & GEF_ONSTACK) && guest_onstack(p)))
		sigaddset(&p->p_hold, e->ge_sig);
	n = __amix_fsig(p);
	p->p_hold = h;
	guest_splx(x);
	return n;
}

static int
genv_sendsig(gp, sig, sip, hdlr)
	struct guest_proc *gp;
	int sig;
	char *sip;
	int (*hdlr)();
{
	if (sig != GENV(gp)->ge_sig)
		return __amix_sendsig(sig, sip, hdlr);
	guest_env_intr(gp, (char *)u.u_ar0);
	return 1;
}

static int
genv_ready(s, m)
	struct guest_sess *s;
	unsigned long m;
{
	if (m & GH_IPL)
		return guest_env_level(s) > GUEST_VIPL(s->gs_gp);
	return (s->gs_pend & m) != 0;
}

/* a stopped guest sleeps until a level it waits for asks */
void
guest_env_after(s)
	struct guest_sess *s;
{
	unsigned long m;
	int x;

	if (s && (m = s->gs_halt) != 0) {
		s->gs_halt = 0;
		x = guest_splhi();
		while (s->gs_state == GS_RUN && !genv_ready(s, m)) {
			s->gs_sleeping = 1;
			if (sleep((caddr_t)&s->gs_sleeping, (PZERO + 1) | PCATCH))
				break;
		}
		s->gs_sleeping = 0;
		if (s->gs_gp)
			s->gs_gp->gp_flags |= GPF_IDROP;
		guest_splx(x);
	}
	guest_trapret();
}

static void
genv_tick(arg)
	caddr_t arg;
{
	struct guest_sess *s = (struct guest_sess *)arg;
	int x = guest_splhi();

	s->gs_tid = 0;
	if (s->gs_state != GS_RUN) {
		guest_splx(x);
		return;
	}
	(*s->gs_env->ge_tick)(s);
	guest_env_kick(s);
	guest_splx(x);
	if ((s->gs_tid = ttimeout(genv_tick, (caddr_t)s, 1L)) == -1)
		s->gs_tid = 0;
}

static void
genv_exit(gp)
	struct guest_proc *gp;
{
	struct guest_sess *s = guest_env_sess(gp);

	if (!s)
		return;
	if (s->gs_tid > 0)
		untimeout(s->gs_tid);
	s->gs_tid = 0;
	if (s->gs_env->ge_exit)
		(*s->gs_env->ge_exit)(s);
	guest_cpage_free(s);
	s->gs_state = GS_FREE;
	s->gs_proc = 0;
	s->gs_gp = 0;
	if (s->gs_sleeping)
		wakeup((caddr_t)&s->gs_sleeping);
}

static int
genv_fork(parent, child)
	struct guest_proc *parent, *child;
{
	return EINVAL;
}

/* a stub of the host-call gate: the service, then past the trap */
static int
genv_hostgate(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	struct guest_env *e = GENV(gp);
	struct guest_sess *s = guest_env_sess(gp);
	unsigned long o = GR_PC(r) - e->ge_hostva;

	if (s && s->gs_state == GS_RUN && o < (unsigned long)e->ge_hostn * e->ge_hoststep &&
	    o % e->ge_hoststep == 0 && (*e->ge_hostcall)(s, r, (int)(o / e->ge_hoststep)) == 0) {
		GR_PC(r) += e->ge_hostlen;
		guest_env_after(s);
		return 0;
	}
	return e->ge_hostdisp.gd_fn ? (*e->ge_hostdisp.gd_fn)(gp, r, v) : 1;
}

/* the claimed session becomes the caller's */
static int
genv_enter(e, arg)
	struct guest_env *e;
	caddr_t arg;
{
	struct guest_sess *s = 0;
	struct guest_proc *gp;
	int i, x, err;

	if (GUESTP(curproc))
		return EBUSY;
	x = guest_splhi();
	for (i = 0; i < e->ge_nsess; i++)
		if (e->ge_sess[i].gs_state == GS_FREE) {
			s = &e->ge_sess[i];
			s->gs_state = GS_CLAIMED;
			break;
		}
	guest_splx(x);
	if (!s)
		return EBUSY;
	s->gs_proc = 0;
	s->gs_gp = 0;
	if ((err = guest_attach(&e->ge_prof)) != 0) {
		s->gs_state = GS_FREE;
		return err;
	}
	gp = GUESTP(curproc);
	gp->gp_vsr = 0x2700;
	gp->gp_vpend = 0;
	gp->gp_vusp = gp->gp_vvbr = gp->gp_vcacr = 0;
	gp->gp_flags |= e->ge_gpflags;
	s->gs_proc = curproc;
	s->gs_gp = gp;
	s->gs_pend = s->gs_halt = s->gs_ints = 0;
	s->gs_sleeping = 0;
	if (e->ge_ssize)
		bzero(s->gs_priv, (u_int)e->ge_ssize);
	if (e->ge_enter && (err = (*e->ge_enter)(s, arg)) != 0) {
		(void)guest_detach(&e->ge_prof);
		return err;
	}
	s->gs_state = GS_RUN;
	if (e->ge_tick && (s->gs_tid = ttimeout(genv_tick, (caddr_t)s, 1L)) == -1)
		s->gs_tid = 0;
	return 0;
}

/* the device's entries, called from the environment's own (a driver's must be in its module) */
int
guest_env_open(e, devp, flags, otyp, cr)
	struct guest_env *e;
	dev_t *devp;
	int flags, otyp;
	struct cred *cr;
{
	if (getminor(*devp))
		return ENXIO;
	e->ge_nopen++;
	return 0;
}

/* the last close */
int
guest_env_close(e, dev, flags, otyp, cr)
	struct guest_env *e;
	dev_t dev;
	int flags, otyp;
	struct cred *cr;
{
	e->ge_nopen = 0;
	return 0;
}

/* the caller's session, else the first */
int
guest_env_ioctl(e, dev, cmd, arg, mode, cr, rvp)
	struct guest_env *e;
	dev_t dev;
	int cmd, mode;
	caddr_t arg;
	struct cred *cr;
	int *rvp;
{
	struct guest_proc *gp = GUESTP(curproc);
	struct guest_sess *s = 0;

	if (cmd == e->ge_entercmd)
		return genv_enter(e, arg);
	if (gp && gp->gp_prof == &e->ge_prof)
		s = guest_env_sess(gp);
	if (!s)
		s = &e->ge_sess[0];
	return e->ge_ioctl ? (*e->ge_ioctl)(e, s, cmd, arg, mode, rvp) : EINVAL;
}

int
guest_env_add(e)
	struct guest_env *e;
{
	struct guest_profile *pf = &e->ge_prof;
	struct guest_disp *d;
	char *p;
	int i, n = sizeof (struct guest_sess) + ((e->ge_ssize + 3) & ~3);

	if (e->ge_nsess < 1)
		return EINVAL;
	p = (char *)kmem_zalloc((u_int)(n * e->ge_nsess), KM_SLEEP);
	e->ge_sess = (struct guest_sess *)p;
	for (i = 0; i < e->ge_nsess; i++) {
		e->ge_sess[i].gs_env = e;
		e->ge_sess[i].gs_priv = p + e->ge_nsess * sizeof (struct guest_sess) +
		    i * (n - sizeof (struct guest_sess));
	}
	pf->gpf_name = e->ge_name;
	pf->gpf_wrapper = e->ge_wrapper;
	pf->gpf_privsz = e->ge_privsz;
	pf->gpf_exit = genv_exit;
	if (!pf->gpf_fork)
		pf->gpf_fork = genv_fork;
	pf->gpf_fsig = genv_fsig;
	pf->gpf_sendsig = genv_sendsig;
	pf->gpf_intr = guest_env_intr;
	if (e->ge_hostcall && e->ge_hostn > 0 && e->ge_hoststep > 0) {
		d = &pf->gpf_disp[e->ge_hostvec];
		e->ge_hostdisp = *d;
		d->gd_kind = GD_EMULATE;
		d->gd_fn = genv_hostgate;
	}
	if (e->ge_access) {
		d = &pf->gpf_disp[2];
		e->ge_faultdisp = *d;
		d->gd_kind = GD_EMULATE;
		d->gd_fn = guest_cpage_fault;
		d = &pf->gpf_disp[9];
		e->ge_tracedisp = *d;
		d->gd_kind = GD_EMULATE;
		d->gd_fn = guest_cpage_trace;
	}
	e->ge_nopen = 0;
	return guest_profile_add(pf);
}

int
guest_env_del(e)
	struct guest_env *e;
{
	int i, n = sizeof (struct guest_sess) + ((e->ge_ssize + 3) & ~3);

	if (e->ge_nopen)
		return EBUSY;
	for (i = 0; i < e->ge_nsess; i++)
		if (e->ge_sess[i].gs_state != GS_FREE)
			return EBUSY;
	guest_profile_del(&e->ge_prof);
	kmem_free((_VOID *)e->ge_sess, (u_int)(n * e->ge_nsess));
	e->ge_sess = 0;
	return 0;
}
