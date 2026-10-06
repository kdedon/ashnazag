#include "amiga.h"

extern int nodev(), ttimeout(), untimeout(), fpu_present;
extern void dlm_cacheflush();
extern struct modwrapper amigaguest_wrapper;
extern int amiga_maprom();
__asm__(".weak cputype");
extern long amiga_cpu __asm__("cputype");
/* read at run time: the compiler takes a declared object's address as nonzero */
static long *volatile amiga_cpup = &amiga_cpu;
static struct guest_profile amiga_profile;
static int amiga_nopen, amiga_devflag[1];
/* entered guests, for the doorbell */
#define AMIGA_NGUEST 8
static struct guest_proc *amiga_guests[AMIGA_NGUEST];
/* doorbell writes per slot, and those AMIGAIOC_WAIT has returned for */
static unsigned long amiga_rung[AMIGA_NGUEST], amiga_heard[AMIGA_NGUEST];

int
amiga_spl()
{
	int s;
	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}
void
amiga_splx(s)
	int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}
static int
amiga_level(gp)
	struct guest_proc *gp;
{
	return amigadev_ipl(&AMIGAP(gp)->ac_dev);
}
static void
amiga_intr(gp, r)
	struct guest_proc *gp;
	char *r;
{
	int s = amiga_spl(), l = amiga_level(gp);
	gp->gp_vpend = l << 8;
	amiga_splx(s);
	/* the frame goes to guest memory, which may fault */
	if (l > ((gp->gp_vsr >> 8) & 7)) {
		if (guest_reflect(gp, r, GR_PC(r), (24 + l) << 2,
		    (char *)0, 2, l) < 0)
			psignal(curproc, SIGSEGV);
		else {
			AMIGAP(gp)->ac_stat.as_intr++;
			if (GR_FV(r) >> 12)
				u.u_sigflag |= USTKCLEAR;
		}
	}
	s = amiga_spl();
	l = amiga_level(gp);
	gp->gp_vpend = l << 8;
	if (l <= ((gp->gp_vsr >> 8) & 7))
		sigdelset(&gp->gp_proc->p_sig, AMIGA_SIG);
	amiga_splx(s);
}
static int
amiga_fsig(p, gp)
	struct proc *p;
	struct guest_proc *gp;
{
	k_sigset_t h;
	int s = amiga_spl(), n;
	h = p->p_hold;
	sigdelset(&p->p_hold, AMIGA_SIG);
	if (amiga_level(gp) <= ((gp->gp_vsr >> 8) & 7))
		sigaddset(&p->p_hold, AMIGA_SIG);
	n = __amix_fsig(p);
	p->p_hold = h;
	amiga_splx(s);
	return n;
}
static int
amiga_sendsig(gp, sig, sip, hdlr)
	struct guest_proc *gp;
	int sig;
	char *sip;
	int (*hdlr)();
{
	if (sig != AMIGA_SIG)
		return __amix_sendsig(sig, sip, hdlr);
	amiga_intr(gp, (char *)u.u_ar0);
	return 1;
}
/* raised by the tick or a helper's doorbell: wake or signal the guest */
static void
amiga_post(gp)
	struct guest_proc *gp;
{
	struct amigactr *a = AMIGAP(gp);
	gp->gp_vpend = amiga_level(gp) << 8;
	if (gp->gp_vpend > (gp->gp_vsr & 0x700)) {
		if (a->ac_sleeping)
			wakeup((caddr_t)&a->ac_sleeping);
		else
			psignal(gp->gp_proc, AMIGA_SIG);
	}
}
void
amiga_ring(gp)
	struct guest_proc *gp;
{
	int i, s = amiga_spl();
	for (i = 0; i < AMIGA_NGUEST; i++)
		if (amiga_guests[i] == gp) {
			amiga_rung[i]++;
			wakeup((caddr_t)&amiga_rung[i]);
		}
	amiga_splx(s);
}
static void
amiga_tick(arg)
	caddr_t arg;
{
	struct guest_proc *gp = (struct guest_proc *)arg;
	struct amigactr *a = AMIGAP(gp);
	unsigned long clocks;
	int s = amiga_spl();
	a->ac_timer = -1;
	a->ac_fraction += (a->ac_config.ae_flags & AMIGAF_PAL) ? 709379 : 715909;
	clocks = a->ac_fraction / HZ;
	a->ac_fraction %= HZ;
	amigadev_tick(&a->ac_dev, clocks);
	a->ac_epoch++;
	amiga_post(gp);
	a->ac_timer = ttimeout(amiga_tick, arg, 1L);
	if (a->ac_timer == -1) {
		psignal(gp->gp_proc, SIGTERM);
		if (a->ac_sleeping) wakeup((caddr_t)&a->ac_sleeping);
	}
	amiga_splx(s);
}
static void
amiga_census(a)
	struct amigactr *a;
{
	struct amigacensus *c = &a->ac_census;
	int i, j;
	printf("amiga census: fast INTENA writes %d\n", (int)amiga_nfast);
	printf("amiga census: faults %d intr %d stop %d last pc %x addr %x\n",
	    (int)a->ac_stat.as_fault, (int)a->ac_stat.as_intr, (int)a->ac_stat.as_stop,
	    (int)a->ac_stat.as_lastpc, (int)a->ac_stat.as_lastaddr);
	for (i = 0; i < 256; i++)
		if (c->custom[i][0] | c->custom[i][1])
			printf("amiga census: %x r %d w %d\n", 0xdff000 + i * 2,
			    (int)c->custom[i][0], (int)c->custom[i][1]);
	for (j = 0; j < 2; j++)
		for (i = 0; i < 16; i++)
			if (c->cia[j][i][0] | c->cia[j][i][1])
				printf("amiga census: %x r %d w %d\n",
				    (j ? 0xbfd000 : 0xbfe001) + i * 256,
				    (int)c->cia[j][i][0], (int)c->cia[j][i][1]);
	for (i = 0; i < AMIGA_NOTHER && c->other[i][1]; i++)
		printf("amiga census: %x n %d pc %x\n", (int)c->other[i][0],
		    (int)c->other[i][1], (int)c->other[i][2]);
}
static void
amiga_exit(gp)
	struct guest_proc *gp;
{
	struct amigactr *a = AMIGAP(gp);
	int s, i;
	if (a->ac_gp == gp && (a->ac_config.ae_flags & AMIGAF_CENSUS))
		amiga_census(a);
	s = amiga_spl();
	if (a->ac_gp == gp && a->ac_timer >= 0)
		untimeout(a->ac_timer);
	if (a->ac_gp == gp)
		sigdelset(&gp->gp_proc->p_sig, AMIGA_SIG);
	for (i = 0; i < AMIGA_NGUEST; i++)
		if (amiga_guests[i] == gp) {
			amiga_guests[i] = 0;
			wakeup((caddr_t)&amiga_rung[i]);
		}
	amiga_splx(s);
}
static int
amiga_fork(pg, cg)
	struct guest_proc *pg, *cg;
{
	return EINVAL;
}
static int
amiga_refl(gp, r, v)
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
	if (guest_reflect(gp, r, GR_PC(r), (GR_FV(r) & 0xf000) | (v << 2),
	    r + 72, n + 2, -1) < 0)
		psignal(curproc, SIGSEGV);
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}
static int
amiga_priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	struct amigactr *a = AMIGAP(gp);
	unsigned char b[4];
	unsigned long *cp, *rp, value;
	int op, ext, c, s;
	if (!(gp->gp_vsr & 0x2000))
		return amiga_refl(gp, r, v);
	a->ac_stat.as_priv++;
	a->ac_stat.as_lastpc = GR_PC(r);
	if (copyin((caddr_t)GR_PC(r), (caddr_t)b, 2))
		return 1;
	op = G16(b);
	if (op == 0x4e70) {
		s = amiga_spl();
		amigadev_reset(&a->ac_dev);
		amigadev_configure(&a->ac_dev, (a->ac_config.ae_flags & AMIGAF_PAL) != 0);
		bzero((caddr_t)a->ac_gary, sizeof a->ac_gary);
		a->ac_epoch++;
		gp->gp_vpend = 0; a->ac_fraction = 0;
		GR_PC(r) += 2;
		sigdelset(&gp->gp_proc->p_sig, AMIGA_SIG);
		amiga_splx(s);
		guest_trapret();
		return 0;
	}
	if (op == 0x4e72) {
		if (copyin((caddr_t)GR_PC(r) + 2, (caddr_t)b, 2))
			return 1;
		guest_setsr(gp, r, (int)G16(b));
		GR_PC(r) += 4;
		a->ac_stat.as_stop++;
		s = amiga_spl();
		while (a->ac_timer >= 0 && amiga_level(gp) <= ((gp->gp_vsr >> 8) & 7)) {
			a->ac_sleeping = 1;
			if (sleep((caddr_t)&a->ac_sleeping, (PZERO + 1) | PCATCH))
				break;
		}
		a->ac_sleeping = 0;
		amiga_splx(s);
		guest_trapret();
		return 0;
	}
	if ((op & 0xfffe) == 0x4e7a) {
		if (copyin((caddr_t)GR_PC(r) + 2, (caddr_t)b, 2))
			return 1;
		ext = G16(b); c = ext & 0xfff; cp = 0;
		if (c == 2) cp = &gp->gp_vcacr;
		if (c >= 3 && c <= 7) cp = &a->ac_mmu[c - 3];
		if (c == 0x805 || c == 0x806 || c == 0x807) cp = &a->ac_mmu[c - 0x800];
		if (c == 0x803 || c == 0x804)
			return amiga_refl(gp, r, 4);
		if (cp) {
			rp = ext >> 12 == 15 ? (unsigned long *)&GR_USP(r) :
			    (unsigned long *)(r + 4 + (ext >> 12) * 4);
			if (op & 1) {
				value = *rp;
				if (c == 2) {
					value &= (amiga_cpup && *amiga_cpup >= 40) ? 0x80008000UL : 0x00003f1fUL;
					dlm_cacheflush();
				}
				*cp = value;
			} else *rp = *cp;
			GR_PC(r) += 4;
			guest_trapret();
			return 0;
		}
	}
	if (guest_priv(gp, r, v) == 0)
		return 0;
	return amiga_refl(gp, r, 4);
}
static int
amiga_fline(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned char b[2];
	if (copyin((caddr_t)GR_PC(r), (caddr_t)b, 2) == 0 &&
	    ((G16(b) >> 9) & 7) == 1 && fpu_present)
		return 1;
	return amiga_refl(gp, r, v);
}
int
amigaopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	if (getminor(*devp)) return ENXIO;
	amiga_nopen++;
	return 0;
}
int
amigaclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	amiga_nopen = 0;
	return 0;
}
int
amigaioctl(dev, cmd, arg, mode, cr, rvp)
	dev_t dev;
	int cmd, mode;
	caddr_t arg;
	struct cred *cr;
	int *rvp;
{
	struct amigaenter ae;
	struct amigainfo ai;
	struct guest_proc *gp;
	struct amigactr *a;
	int e, i, s;
	if (cmd == AMIGAIOC_KICK || cmd == AMIGAIOC_WAIT) {
		s = amiga_spl();
		for (i = 0; i < AMIGA_NGUEST; i++) {
			gp = amiga_guests[i];
			if (gp && gp->gp_proc->p_pid == (pid_t)arg &&
			    (gp->gp_proc->p_cred->cr_uid == cr->cr_uid || cr->cr_uid == 0))
				break;
		}
		e = i < AMIGA_NGUEST ? 0 : ESRCH;
		if (!e && cmd == AMIGAIOC_KICK) {
			/* a helper's doorbell: PORTS, as an expansion board raises it */
			AMIGAP(gp)->ac_dev.intreq |= 0x0008;
			AMIGAP(gp)->ac_epoch++;
			amiga_post(gp);
		} else if (!e) {
			/* the slot may be reused while asleep: the caller asks again */
			if (amiga_rung[i] == amiga_heard[i] &&
			    sleep((caddr_t)&amiga_rung[i], (PZERO + 1) | PCATCH))
				e = EINTR;
			else
				amiga_heard[i] = amiga_rung[i];
		}
		amiga_splx(s);
		return e;
	}
	if (cmd == AMIGAIOC_INFO) {
		ai.ai_version = AMIGA_ABI_VERSION;
		ai.ai_features = AMIGA_FEAT_BASE | AMIGA_FEAT_KICK;
		if (amiga_cpup && *amiga_cpup == 40) ai.ai_features |= AMIGA_FEAT_EXPERIMENTAL;
		return copyout((caddr_t)&ai, arg, sizeof ai) ? EFAULT : 0;
	}
	if (cmd == AMIGAIOC_MAPROM)
		return GUESTP(curproc) && GUESTP(curproc)->gp_prof != &amiga_profile ?
		    EPERM : amiga_maprom(dev, rvp);
	if (cmd == AMIGAIOC_ENTER) {
		if (copyin(arg, (caddr_t)&ae, sizeof ae)) return EFAULT;
		if (ae.ae_version != AMIGA_ABI_VERSION || ae.ae_chipsize != AMIGA_CHIP_SIZE ||
		    ae.ae_fastsize > AMIGA_FAST_MAX || (ae.ae_fastsize & 0xfffff) ||
		    (ae.ae_flags & ~(AMIGAF_PAL | AMIGAF_CENSUS))) return EINVAL;
		/* the census goes to the console */
		if ((ae.ae_flags & AMIGAF_CENSUS) && !suser(cr)) return EPERM;
		if ((e = guest_attach(&amiga_profile)) != 0) return e;
		gp = GUESTP(curproc); a = AMIGAP(gp);
		a->ac_gp = gp; a->ac_config = ae;
		a->ac_stat.as_version = AMIGA_ABI_VERSION;
		a->ac_stat.as_pid = curproc->p_pid;
		amigadev_reset(&a->ac_dev);
		amigadev_configure(&a->ac_dev, (ae.ae_flags & AMIGAF_PAL) != 0);
		gp->gp_vsr = 0x2700;
		gp->gp_flags = GPF_PRIV | GPF_FTRAP | GPF_FTRAP13;
		dlm_cacheflush();
		a->ac_timer = ttimeout(amiga_tick, (caddr_t)gp, 1L);
		if (a->ac_timer == -1) {
			guest_detach(&amiga_profile);
			return EAGAIN;
		}
		s = amiga_spl();
		for (i = 0; i < AMIGA_NGUEST && amiga_guests[i]; i++)
			;
		if (i < AMIGA_NGUEST) {
			amiga_guests[i] = gp;
			amiga_heard[i] = amiga_rung[i];
		}
		amiga_splx(s);
		return 0;
	}
	gp = GUESTP(curproc);
	if (!gp || gp->gp_prof != &amiga_profile) return ENXIO;
	if (cmd == AMIGAIOC_LEAVE) return guest_detach(&amiga_profile);
	if (cmd == AMIGAIOC_STAT)
		return copyout((caddr_t)&AMIGAP(gp)->ac_stat, arg, sizeof(struct amigastat)) ? EFAULT : 0;
	return EINVAL;
}
static int
amigaguest_load()
{
	int v;
	struct guest_disp *d;
	amiga_profile.gpf_name = "amiga";
	amiga_profile.gpf_wrapper = &amigaguest_wrapper;
	amiga_profile.gpf_privsz = sizeof(struct amigactr);
	amiga_profile.gpf_exit = amiga_exit;
	amiga_profile.gpf_fork = amiga_fork;
	amiga_profile.gpf_intr = amiga_intr;
	amiga_profile.gpf_fsig = amiga_fsig;
	amiga_profile.gpf_sendsig = amiga_sendsig;
	for (v = 2; v < 48; v++) {
		if ((v > 11 && v < 32) || v == 32) continue;
		d = &amiga_profile.gpf_disp[v];
		d->gd_flags = GDF_USER;
		d->gd_kind = GD_EMULATE;
		d->gd_fn = v == 2 ? amiga_fault : v == 8 ? amiga_priv : v == 11 ? amiga_fline : amiga_refl;
	}
	return guest_profile_add(&amiga_profile);
}
static int
amigaguest_unload()
{
	if (amiga_nopen) return EBUSY;
	guest_profile_del(&amiga_profile);
	return 0;
}
struct mod_drv_data amigaguest_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	  { amigaopen, amigaclose, nodev, nodev, amigaioctl, nodev, nodev, nodev,
	    nodev, nodev, 0, 0, amiga_devflag }, AMIGA_MAJOR, 1 }
};
MOD_DRV_WRAPPER(amigaguest, amigaguest_load, amigaguest_unload, 0, "Amiga guest");
