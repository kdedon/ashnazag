/*
 * tosguest.c -- /dev/tos and the TOS profile.
 *
 * TOS runs natively in user mode in the launcher's process, with the
 * supervisor state virtual (guestcore).  Traps, line A and line F, and
 * the other exceptions go through the guest's own vector table; the
 * register windows are emulated (tosdev.c).  Interrupts are virtual:
 * the MFP's timers and the VBL advance on the clock tick, the IKBD
 * ACIA takes bytes from the container's display process, and TOS_SIG
 * carries them, held while the guest's IPL masks them; on delivery
 * the sendsig hook builds the 68k interrupt frame instead of a Unix one.
 *
 * K&R C.
 */

#include "tos.h"

extern int nodev(), ttimeout(), untimeout();
extern char runrun;
extern int fpu_present;
extern void dlm_cacheflush();
extern long lbolt;
extern struct modwrapper tosguest_wrapper;

struct tosctr tosc;
int	tos_trace = 0;		/* 1: console lines for bus errors and odd accesses */

static struct guest_profile tos_profile;
static int tos_nopen;
static int tosdevflag[1] = { 0 };

int
splhi_()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}

void
splx_(s)
	int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

/* the highest level asking, 0 none */
static int
tos_level()
{
	if (mfp_level())
		return 6;
	return tosc.t_vblpend ? 4 : 0;
}

static int
vipl(gp)
	struct guest_proc *gp;
{
	return (gp->gp_vsr & SR_IPL) >> 8;
}

/*
 * Something may want the guest: post the carrier.  rr: also reschedule
 * at once when the guest can take it now.
 */
void
tos_kick(rr)
	int rr;
{
	struct tosctr *t = &tosc;
	int l;

	if (t->t_state != 1 || (l = tos_level()) == 0)
		return;
	psignal(t->t_proc, TOS_SIG);
	if (l > vipl(t->t_gp)) {
		if (t->t_sleeping)
			wakeup((caddr_t)&t->t_sleeping);
		else if (rr)
			runrun = 1;
	}
}

/* stop: the guest sleeps until an interrupt above its new IPL is pending */
static void
tos_stop_insn(gp)
	struct guest_proc *gp;
{
	struct tosctr *t = &tosc;
	int s = splhi_();
	long t0 = lbolt;

	t->t_st.ts_stop++;
	while (t->t_state == 1 && tos_level() <= vipl(gp)) {
		t->t_sleeping = 1;
		if (sleep((caddr_t)&t->t_sleeping, (PZERO + 1) | PCATCH))
			break;
	}
	t->t_sleeping = 0;
	t->t_st.ts_slept += lbolt - t0;
	splx_(s);
}

/* ---- delivery ---- */

/* the carrier waits while the guest's IPL masks every request */
static int
tos_fsig(p, gp)
	struct proc *p;
	struct guest_proc *gp;
{
	k_sigset_t h;
	int s, n;

	s = splhi_();
	h = p->p_hold;
	sigdelset(&p->p_hold, TOS_SIG);
	if (tosc.t_gp != gp || tos_level() <= vipl(gp))
		sigaddset(&p->p_hold, TOS_SIG);
	n = __amix_fsig(p);
	p->p_hold = h;
	splx_(s);
	return n;
}

/* the carrier: one interrupt frame through the guest's vector table */
static int
tos_sendsig(gp, sig, sip, hdlr)
	struct guest_proc *gp;
	int sig;
	char *sip;
	int (*hdlr)();
{
	char *r = (char *)u.u_ar0;
	int s, l, vec;

	if (sig != TOS_SIG)
		return __amix_sendsig(sig, sip, hdlr);
	if (tosc.t_gp != gp)
		return 1;
	s = splhi_();
	l = tos_level();
	if (l <= vipl(gp)) {
		splx_(s);
		return 1;
	}
	if (l == 6)
		vec = mfp_ack();
	else {
		tosc.t_vblpend = 0;
		tosc.t_st.ts_vbl++;
		vec = 28;
	}
	splx_(s);
	if (guest_reflect(gp, r, GR_PC(r), vec << 2, (char *)0, 2, l) < 0) {
		s = splhi_();
		if (l == 6)
			mfp_unack(vec);		/* still pending for a debugger */
		else
			tosc.t_vblpend = 1;
		splx_(s);
		printf("tos: interrupt with no guest stack, pc %x\n", (int)GR_PC(r));
		psignal(curproc, SIGSEGV);
		return 1;
	}
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	tos_kick(0);
	return 1;
}

/* ---- dispositions ---- */

/* the CPU's own frame, through the guest's vector */
static int
tos_refl(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int n;

	switch (GR_FV(r) >> 12) {
	case 0: n = 0; break;
	case 2: case 3: n = 4; break;
	case 4: n = 8; break;
	default: return 1;
	}
	tosc.t_st.ts_lastpc = GR_PC(r);
	if (v < 64)
		tosc.t_st.ts_refl[v]++;
	if (guest_reflect(gp, r, GR_PC(r), (int)GR_FV(r), r + 72, n + 2, -1) < 0) {
		printf("tos: vector %d with no guest stack, pc %x\n", v, (int)GR_PC(r));
		psignal(curproc, SIGSEGV);
	}
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

/* a frame of our own: format 0 at pc */
static int
tos_refl0(gp, r, v, pc)
	struct guest_proc *gp;
	char *r;
	int v;
	long pc;
{
	if (v < 64)
		tosc.t_st.ts_refl[v]++;
	if (guest_reflect(gp, r, pc, v << 2, (char *)0, 2, -1) < 0) {
		printf("tos: vector %d with no guest stack, pc %x\n", v, (int)pc);
		psignal(curproc, SIGSEGV);
	}
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

static int
opword(r)
	char *r;
{
	char b[2];

	if (copyin((caddr_t)GR_PC(r), b, 2))
		return -1;
	return (int)G16(b);
}

/* vector 8: user mode reflects; reset is a no-op; the rest guestcore */
static int
tos_priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	char sr[2];

	tosc.t_st.ts_lastpc = GR_PC(r);
	if (!(gp->gp_vsr & SR_S))
		return tos_refl(gp, r, v);
	tosc.t_st.ts_priv++;
	if (opword(r) == 0x4e70) {		/* reset */
		GR_PC(r) += 2;
		guest_trapret();
		return 0;
	}
	if (opword(r) == 0x4e72 && copyin((caddr_t)GR_PC(r) + 2, sr, 2) == 0) {
		guest_setsr(gp, r, (int)G16(sr));
		GR_PC(r) += 4;
		tos_stop_insn(gp);
		guest_trapret();
		return 0;
	}
	if (opword(r) >> 8 == 0xf4 || (opword(r) == 0x4e7b &&
	    copyin((caddr_t)GR_PC(r) + 2, sr, 2) == 0 && (G16(sr) & 0xfff) == 2))
		tosc.t_st.ts_cache++;	/* cinv, cpush, CACR: pushed for real */
	if (guest_priv(gp, r, v) == 0)
		return 0;
	return tos_refl0(gp, r, 4, GR_PC(r));	/* illegal instruction */
}

/* vector 11: 68030 MMU instructions emulated; FPU to the host */
static int
tos_fline(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	extern int tos_mmu();
	int op = opword(r), n, sr;
	char save[64];

	tosc.t_st.ts_lastpc = GR_PC(r);
	if ((op & 0xffc0) == 0xf000) {
		if (!(gp->gp_vsr & SR_S))
			return tos_refl0(gp, r, 8, GR_PC(r));
		bcopy(r, save, sizeof save);
		sr = GR_SR(r);
		if ((n = tos_mmu(gp, r, (unsigned long)op)) <= 0) {
			bcopy(save, r, sizeof save);	/* undo (An)+ and -(An) */
			GR_SR(r) = sr;
		} else {
			tosc.t_st.ts_mmu++;
			GR_PC(r) += n;
			if (GR_FV(r) >> 12)
				u.u_sigflag |= USTKCLEAR;
			guest_trapret();
			return 0;
		}
	}
	if (((op >> 9) & 7) == 1 && fpu_present)
		return 1;
	return tos_refl(gp, r, v);
}

/* trap #0: a host system call from the machine layer */
static int
tos_sys(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	tosc.t_st.ts_sys++;
	return 1;
}

/* ---- the tick ---- */

static void
tos_tick(arg)
	caddr_t arg;
{
	struct tosctr *t = &tosc;
	int s;

	t->t_tid = 0;
	if (t->t_state != 1)
		return;
	s = splhi_();
	t->t_vblpend = 1;
	tos_timers();
	splx_(s);
	tos_kick(1);
	if ((t->t_tid = ttimeout(tos_tick, (caddr_t)0, 1L)) == -1)
		t->t_tid = 0;
}

static void
tos_stop()
{
	struct tosctr *t = &tosc;
	int s = splhi_();

	if (t->t_tid) {
		untimeout(t->t_tid);
		t->t_tid = 0;
	}
	t->t_state = 0;
	t->t_proc = 0;
	t->t_gp = 0;
	splx_(s);
}

static void
tos_pexit(gp)
	struct guest_proc *gp;
{
	if (tosc.t_gp == gp)
		tos_stop();
}

static int
tos_pfork(pg, cg)
	struct guest_proc *pg, *cg;
{
	return EINVAL;			/* one process per container */
}

/* ---- /dev/tos ---- */

int
tosopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	if (getminor(*devp) != 0)
		return ENXIO;
	tos_nopen++;
	return 0;
}

int
tosclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	tos_nopen = 0;			/* called on the last close only */
	return 0;
}

int
tosrdwr(dev, uiop, cr)
	dev_t dev;
	struct uio *uiop;
	struct cred *cr;
{
	return EINVAL;
}

static int
enter(arg, cr)
	caddr_t arg;
	struct cred *cr;
{
	struct tosctr *t = &tosc;
	struct tosenter te;
	struct guest_proc *gp;
	int e, s;

	if (copyin(arg, (caddr_t)&te, sizeof te))
		return EFAULT;
	if (te.te_ramsize < 0x80000 || te.te_ramsize > 0xe00000 || (te.te_ramsize & 0xfff))
		return EINVAL;
	s = splhi_();
	if (t->t_state != 0) {
		splx_(s);
		return EBUSY;
	}
	t->t_state = 2;			/* claimed: the tick and hooks wait for 1 */
	splx_(s);
	if ((e = guest_attach(&tos_profile)) != 0) {
		t->t_state = 0;
		return e;
	}
	gp = GUESTP(curproc);
	gp->gp_flags |= GPF_PRIV;
	gp->gp_vsr = 0x2700;
	gp->gp_vusp = gp->gp_vvbr = gp->gp_vcacr = 0;
	bzero((caddr_t)&t->t_proc, sizeof *t - ((caddr_t)&t->t_proc - (caddr_t)t));
	t->t_proc = curproc;
	t->t_gp = gp;
	t->t_uid = cr->cr_uid;
	t->t_ramsize = te.te_ramsize;
	t->t_flags = te.te_flags;
	t->t_st.ts_pid = curproc->p_pid;
	tos_devinit(t);
	dlm_cacheflush();		/* the launcher's ROM and cartridge stores reach memory */
	t->t_state = 1;
	if ((t->t_tid = ttimeout(tos_tick, (caddr_t)0, 1L)) == -1)
		t->t_tid = 0;
	return 0;
}

int
tosioctl(dev, cmd, arg, mode, cr, rvp)
	dev_t dev;
	int cmd, mode;
	caddr_t arg;
	struct cred *cr;
	int *rvp;
{
	struct tosctr *t = &tosc;
	struct tosinput ti;
	struct tosvideo *tv;
	int e, s;

	if (cmd == TOSIOC_ENTER)
		return enter(arg, cr);
	if (cmd == TOSIOC_OWNER) {
		struct tosowner to;

		to.to_pid = t->t_state ? t->t_st.ts_pid : 0;
		to.to_uid = t->t_state ? t->t_uid : 0;
		return copyout((caddr_t)&to, arg, sizeof to) ? EFAULT : 0;
	}
	if (t->t_state != 1)
		return ENXIO;
	if (cr->cr_uid != t->t_uid && cr->cr_uid != 0)
		return EPERM;
	switch (cmd) {
	case TOSIOC_INPUT:
		if (copyin(arg, (caddr_t)&ti, sizeof ti))
			return EFAULT;
		if (ti.ti_n < 0 || ti.ti_n > sizeof ti.ti_b)
			return EINVAL;
		tos_input(&ti);
		return 0;
	case TOSIOC_VIDEO:
		tv = (struct tosvideo *)kmem_zalloc(sizeof *tv, KM_SLEEP);
		s = splhi_();
		tv->tv_gen = t->t_vgen;
		tv->tv_base = t->t_vid[1] << 16 | t->t_vid[3] << 8 | t->t_vid[0x0d];
		tv->tv_stmode = t->t_vid[0x60];
		tv->tv_ttmode = t->t_vid[0x62] << 8 | t->t_vid[0x63];
		bcopy((caddr_t)t->t_vid + 0x40, (caddr_t)tv->tv_stpal, sizeof tv->tv_stpal);
		bcopy((caddr_t)t->t_ttpal, (caddr_t)tv->tv_ttpal, sizeof tv->tv_ttpal);
		tv->tv_vbl = t->t_st.ts_vbl;
		splx_(s);
		e = copyout((caddr_t)tv, arg, sizeof *tv) ? EFAULT : 0;
		kmem_free((caddr_t)tv, sizeof *tv);
		return e;
	case TOSIOC_STAT:
		return copyout((caddr_t)&t->t_st, arg, sizeof t->t_st) ? EFAULT : 0;
	}
	return EINVAL;
}

/* ---- module ---- */

static int
tosguest_load()
{
	struct guest_disp *d;
	int v;

	tos_profile.gpf_name = "tos";
	tos_profile.gpf_wrapper = &tosguest_wrapper;
	tos_profile.gpf_exit = tos_pexit;
	tos_profile.gpf_fork = tos_pfork;
	tos_profile.gpf_sendsig = tos_sendsig;
	tos_profile.gpf_fsig = tos_fsig;
	for (v = 2; v < 48; v++) {
		if (v > 11 && v < 32)
			continue;
		d = &tos_profile.gpf_disp[v];
		d->gd_flags = GDF_USER;
		d->gd_kind = v == 2 || v == 8 || v == 11 ? GD_EMULATE : GD_REFLECT;
		d->gd_fn = v == 2 ? tos_fault : v == 8 ? tos_priv : v == 11 ? tos_fline : tos_refl;
		if (v == 32) {		/* host system calls, counted */
			d->gd_kind = GD_SYSCALL;
			d->gd_fn = tos_sys;
		}
	}
	return guest_profile_add(&tos_profile);
}

static int
tosguest_unload()
{
	if (tosc.t_state != 0 || tos_nopen)
		return EBUSY;
	guest_profile_del(&tos_profile);
	return 0;
}

struct mod_drv_data tosguest_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	  { tosopen, tosclose, tosrdwr, tosrdwr, tosioctl, nodev, nodev, nodev,
	    nodev, nodev, 0, 0, tosdevflag }, TOS_MAJOR, 1 }
};

MOD_DRV_WRAPPER(tosguest, tosguest_load, tosguest_unload, 0, "Atari TOS guest");
