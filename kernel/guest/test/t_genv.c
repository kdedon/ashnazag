/* host checks: the session layer's carrier loop, kick, stop and host-call gate */
#include "genv.c"

struct t_user u;
struct proc *curproc, proc0;

static struct guest_env env;
static int nfail, nwake, nsleep, nsig, lastsig, nreflect, rfail, nhold, tails, sigtail = 1;
static int nenter, nexit, nhost, hostn, lastvec[8], nattached, ntimeout;
static unsigned long held;
static struct guest_proc *gp0;
static unsigned char r[64 + 92];
static int pend0;

#define	EQ(what, a, b)	((a) == (b) ? 0 : (nfail++, printf("FAIL %s: %ld, want %ld\n", what, (long)(a), (long)(b))))

int wakeup(a) caddr_t a; { nwake++; return 0; }
/* a sleeper wakes to the request the test queued */
int sleep(a, p) caddr_t a; int p; { struct guest_sess *s = (struct guest_sess *)(a - (caddr_t)&((struct guest_sess *)0)->gs_sleeping); nsleep++; s->gs_pend |= pend0; return 0; }
int psignal(p, sig) struct proc *p; int sig; { nsig++; lastsig = sig; return 0; }
int __amix_fsig(p) struct proc *p; { held = p->p_hold; return 0; }
int __amix_sendsig() { return 7; }
int guest_sigtail(x) char *x; { return sigtail; }
int guest_onstack(p) struct proc *p; { return 0; }
void guest_trapret() { tails++; }
int guest_profile_add(pf) struct guest_profile *pf; { return 0; }
void guest_profile_del(pf) struct guest_profile *pf; { }
int ttimeout() { ntimeout++; return 5; }
int untimeout() { return 0; }
void guest_cpage_unsink(s, r) struct guest_sess *s; char *r; { }
void guest_cpage_free(s) struct guest_sess *s; { }
int guest_cpage_fault() { return 1; }
int guest_cpage_trace() { return 1; }

int
guest_attach(pf)
	struct guest_profile *pf;
{
	if (curproc->p_evpdp)
		return EBUSY;
	gp0 = calloc(1, sizeof *gp0);
	gp0->gp_proc = curproc;
	gp0->gp_prof = pf;
	curproc->p_evpdp = (struct evpd *)gp0;
	nattached++;
	return 0;
}

int
guest_detach(pf)
	struct guest_profile *pf;
{
	(*pf->gpf_exit)(gp0);
	curproc->p_evpdp = 0;
	return 0;
}

/* as the CPU: the IPL rises to the interrupt's */
int
guest_reflect(gp, x, pc, fv, t, n, ipl)
	struct guest_proc *gp;
	char *x, *t;
	long pc;
	int fv, n, ipl;
{
	if (rfail)
		return -1;
	lastvec[nreflect++ & 7] = fv >> 2;
	if (ipl >= 0)
		gp->gp_vsr = (gp->gp_vsr & ~0x700) | ipl << 8;
	return 0;
}

static int t_enter(s, a) struct guest_sess *s; caddr_t a; { nenter++; return a ? EFAULT : 0; }
static void t_exit(s) struct guest_sess *s; { nexit++; }
static void t_tick(s) struct guest_sess *s; { s->gs_pend |= 2; }
static int t_sync(s) struct guest_sess *s; { return 1; }
static int t_host(s, x, n) struct guest_sess *s; char *x; int n; { nhost++; hostn = n; return n == 5 ? -1 : 0; }
static int t_disp(gp, x, v) struct guest_proc *gp; char *x; int v; { return 9; }


static struct guest_sess *
start(ipl)
	int ipl;
{
	struct guest_sess *s;

	curproc->p_evpdp = 0;
	EQ("enter", guest_env_ioctl(&env, (dev_t)(65 << 8), 1, (caddr_t)0, 0, (struct cred *)0, (int *)0), 0);
	s = guest_env_sess(gp0);
	gp0->gp_vsr = 0x2000 | ipl << 8;
	nsig = nwake = nreflect = 0;
	memset(r, 0, sizeof r);
	u.u_sigflag = 0;
	u.u_ar0 = (char *)r;
	return s;
}

static void
stop(s)
	struct guest_sess *s;
{
	(void)guest_detach(&env.ge_prof);
	EQ("freed", s->gs_state, GS_FREE);
}

int
main()
{
	struct guest_sess *s;
	int i;

	curproc = &proc0;
	env.ge_name = "test";
	env.ge_major = 65;
	env.ge_entercmd = 1;
	env.ge_ssize = 12;
	env.ge_nsess = 1;
	env.ge_sig = 17;
	env.ge_gpflags = GPF_FRAME68K;
	env.ge_holdfmt = 1 << 0xb;
	env.ge_hostva = 0x1000000;
	env.ge_hostn = 12;
	env.ge_hoststep = 8;
	env.ge_hostvec = 4;
	env.ge_hostlen = 2;
	env.ge_enter = t_enter;
	env.ge_exit = t_exit;
	env.ge_tick = t_tick;
	env.ge_hostcall = t_host;
	env.ge_prof.gpf_disp[4].gd_fn = t_disp;
	EQ("add", guest_env_add(&env), 0);
	EQ("hostgate installed", env.ge_prof.gpf_disp[4].gd_fn == genv_hostgate, 1);

	/* enter: claimed, attached, the hook, a tick; one session only */
	s = start(0);
	EQ("running", s->gs_state, GS_RUN);
	EQ("enter hook", nenter, 1);
	EQ("gp flags", gp0->gp_flags & GPF_FRAME68K, GPF_FRAME68K);
	EQ("tick armed", ntimeout, 1);
	EQ("second enter", guest_env_ioctl(&env, (dev_t)(65 << 8), 1, (caddr_t)0, 0, (struct cred *)0, (int *)0), EBUSY);
	EQ("busy unload", guest_env_del(&env), EBUSY);

	/* two levels at IPL 0: the higher one, which masks the other */
	s->gs_pend = 1 << 1 | 1 << 5;
	proc0.p_sig = 1UL << 16;
	guest_env_intr(gp0, (char *)r);
	EQ("one frame", nreflect, 1);
	EQ("autovector 5", lastvec[0], 29);
	EQ("level 1 waits", s->gs_pend, 1 << 1);
	EQ("vpend", gp0->gp_vpend, 1 << 8);
	EQ("carrier cleared", proc0.p_sig, 0);
	EQ("ints", s->gs_ints, 1);

	/* lower IPL again: level 1 */
	gp0->gp_vsr = 0x2000;
	proc0.p_sig = 1UL << 16;
	guest_env_intr(gp0, (char *)r);
	EQ("level 1", lastvec[1], 25);
	EQ("all taken", s->gs_pend, 0);

	/* a level above the IPL arriving while running is taken on top */
	gp0->gp_vsr = 0x2200;
	s->gs_pend = 1 << 1 | 1 << 3 | 1 << 6;
	nreflect = 0;
	guest_env_intr(gp0, (char *)r);
	EQ("6 only", nreflect, 1);
	EQ("6 first", lastvec[0], 30);

	/* a format $B frame without its instruction finished: nothing */
	gp0->gp_vsr = 0x2000;
	s->gs_pend = 1 << 4;
	nreflect = 0;
	GR_FV(r) = 0xb008;
	guest_env_intr(gp0, (char *)r);
	EQ("held for the frame", nreflect, 0);
	u.u_sigflag = USTKCLEAR;
	guest_env_intr(gp0, (char *)r);
	EQ("frame finished", nreflect, 1);
	GR_FV(r) = 0;
	u.u_sigflag = 0;

	/* no stack: a notice and SIGSEGV, the request kept */
	s->gs_pend = 1 << 2;
	gp0->gp_vsr = 0x2000;
	rfail = 1;
	nsig = 0;
	guest_env_intr(gp0, (char *)r);
	EQ("segv", lastsig, SIGSEGV);
	rfail = 0;

	/* one per carrier, and the signal return check */
	env.ge_flags = GEF_ONE;
	s->gs_pend = 1 << 2 | 1 << 1;
	gp0->gp_vsr = 0x2000;
	nreflect = 0;
	guest_env_intr(gp0, (char *)r);
	EQ("one per carrier", nreflect, 1);
	env.ge_flags = GEF_SIGTAIL;
	sigtail = 0;
	gp0->gp_vsr = 0x2000;
	proc0.p_sig = 1UL << 16;
	guest_env_intr(gp0, (char *)r);
	EQ("not at that frame", nreflect, 1);
	EQ("carrier stays", proc0.p_sig, 1UL << 16);
	sigtail = 1;
	env.ge_flags = 0;

	/* the carrier is held while the IPL masks every request */
	s->gs_pend = 1 << 2;
	gp0->gp_vsr = 0x2300;
	proc0.p_hold = 0;
	(void)genv_fsig(&proc0, gp0);
	EQ("held", held, 1UL << 16);
	EQ("hold restored", proc0.p_hold, 0);
	gp0->gp_vsr = 0x2100;
	(void)genv_fsig(&proc0, gp0);
	EQ("let through", held, 0);
	/* the environment's own mask holds it, and the carrier sends no frame */
	env.ge_sync = t_sync;
	(void)genv_fsig(&proc0, gp0);
	EQ("sync holds", held, 1UL << 16);
	nreflect = 0;
	guest_env_intr(gp0, (char *)r);
	EQ("sync: no frame", nreflect, 0);
	env.ge_sync = 0;
	EQ("other signals", genv_sendsig(gp0, 2, (char *)0, 0), 7);

	/* kick: a signal above the IPL, a wakeup when stopped, nothing below */
	nsig = 0;
	gp0->gp_vsr = 0x2300;
	s->gs_pend = 1 << 2;
	guest_env_kick(s);
	EQ("masked: no signal", nsig, 0);
	guest_env_post(s, 5);
	EQ("signal", nsig, 1);
	EQ("signal number", lastsig, 17);
	s->gs_sleeping = 1;
	guest_env_kick(s);
	EQ("wakeup", nwake, 1);
	EQ("no second signal", nsig, 1);
	s->gs_sleeping = 0;

	/* stop: sleeps until a level of its mask; the tail runs */
	s->gs_pend = 0;
	s->gs_halt = 1 << 6;
	pend0 = 1 << 6;
	nsleep = tails = 0;
	gp0->gp_flags &= ~GPF_IDROP;
	guest_env_after(s);
	EQ("slept once", nsleep, 1);
	EQ("idrop", gp0->gp_flags & GPF_IDROP, GPF_IDROP);
	EQ("tail", tails, 1);
	EQ("halt cleared", s->gs_halt, 0);
	s->gs_halt = GH_IPL;
	s->gs_pend = 1 << 4;
	gp0->gp_vsr = 0x2300;
	nsleep = 0;
	guest_env_after(s);
	EQ("level above the IPL: no sleep", nsleep, 0);

	/* the tick posts and kicks */
	s->gs_pend = 0;
	gp0->gp_vsr = 0x2000;
	nsig = 0;
	genv_tick((caddr_t)s);
	EQ("tick level 1", s->gs_pend, 2);
	EQ("tick carrier", nsig, 1);

	/* host calls: a stub, a declined stub, an odd pc */
	GR_PC(r) = 0x1000000 + 3 * 8;
	EQ("host call", genv_hostgate(gp0, (char *)r, 4), 0);
	EQ("service 3", hostn, 3);
	EQ("past the trap", GR_PC(r), 0x1000000 + 3 * 8 + 2);
	GR_PC(r) = 0x1000000 + 5 * 8;
	EQ("declined", genv_hostgate(gp0, (char *)r, 4), 9);
	GR_PC(r) = 0x1000000 + 3 * 8 + 2;
	i = nhost;
	EQ("between stubs", genv_hostgate(gp0, (char *)r, 4), 9);
	GR_PC(r) = 0x1000000 + 12 * 8;
	EQ("past the stubs", genv_hostgate(gp0, (char *)r, 4), 9);
	EQ("no service", nhost, i);

	/* exit frees the session; a failed enter leaves none */
	stop(s);
	EQ("exit hook", nexit, 1);
	curproc->p_evpdp = 0;
	EQ("failed enter", guest_env_ioctl(&env, (dev_t)(65 << 8), 1, (caddr_t)1, 0, (struct cred *)0, (int *)0), EFAULT);
	EQ("session free", env.ge_sess[0].gs_state, GS_FREE);
	curproc->p_evpdp = 0;
	EQ("minor 1", guest_env_open(&env, &(dev_t){ 1 }, 0, 0, (struct cred *)0), ENXIO);
	EQ("unload", guest_env_del(&env), 0);
	printf(nfail ? "[FAIL] t_genv: %d\n" : "[OK] t_genv\n", nfail);
	return nfail != 0;
}
