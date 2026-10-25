#include "amiga.h"
#include "amigasock.h"
#include "guest_rpage.h"
#include "guest_env.h"

extern int nodev(), ttimeout(), untimeout(), fpu_present, runrun;
extern long lbolt;
extern void dlm_cacheflush();
extern struct modwrapper amigaguest_wrapper;
extern int amiga_maprom(), amiga_sock();
__asm__(".weak cputype");
extern long amiga_cpu __asm__("cputype");
/* read at run time: the compiler takes a declared object's address as nonzero */
static long *__volatile__ amiga_cpup = &amiga_cpu;
static struct guest_env amiga_env;
static int amiga_devflag[1];
/* sessions, each with its doorbell */
#define AMIGA_NGUEST 8
/* doorbell writes per slot, and those AMIGAIOC_WAIT has returned for */
static unsigned long amiga_rung[AMIGA_NGUEST], amiga_heard[AMIGA_NGUEST];
/* the same for the sound doorbell and AMIGAIOC_SNDWAIT */
static unsigned long amiga_srung[AMIGA_NGUEST], amiga_sheard[AMIGA_NGUEST];
/* requests the answering helper has finished; a guest waits for its own */
static unsigned long amiga_answered[AMIGA_NGUEST];
static char amiga_answers[AMIGA_NGUEST], amiga_late[AMIGA_NGUEST];

/* the session's slot, -1 none */
static int
amiga_slot(gp)
	struct guest_proc *gp;
{
	int i;
	for (i = 0; i < AMIGA_NGUEST; i++)
		if (amiga_env.ge_sess[i].gs_state == GS_RUN && amiga_env.ge_sess[i].gs_gp == gp)
			return i;
	return -1;
}
/* the running session of process pid the caller may reach, -1 none */
static int
amiga_pidslot(pid)
	long pid;
{
	struct guest_sess *s;
	struct cred *cr = curproc->p_cred;
	int i;
	for (i = 0; i < AMIGA_NGUEST; i++) {
		s = &amiga_env.ge_sess[i];
		if (s->gs_state == GS_RUN && s->gs_proc->p_pid == (pid_t)pid &&
		    (s->gs_proc->p_cred->cr_uid == cr->cr_uid || cr->cr_uid == 0))
			return i;
	}
	return -1;
}
static void
amiga_post(gp)
	struct guest_proc *gp;
{
	int i = amiga_slot(gp);
	if (i >= 0)
		guest_env_kick(&amiga_env.ge_sess[i]);
}
/* the interrupt level of the host's words; a CIA with unmasked ICR bits holds its INTREQ bit */
static int
amiga_ipl(a)
	struct amigactr *a;
{
	unsigned short p;
	if (a->ac_icr[0] & a->ac_icrmask[0])
		a->ac_intreq |= 0x0008;
	if (a->ac_icr[1] & a->ac_icrmask[1])
		a->ac_intreq |= 0x2000;
	if (!(a->ac_intena & 0x4000))
		return 0;
	p = a->ac_intena & a->ac_intreq;
	if (p & 0x2000) return 6;
	if (p & 0x1800) return 5;
	if (p & 0x0780) return 4;
	if (p & 0x0070) return 3;
	if (p & 0x0008) return 2;
	if (p & 0x0007) return 1;
	return 0;
}
/*
 * The guest's master bit, $4000 or 0, from guest memory; -1 without one.
 * Guest context only.
 */
int
amiga_pvget(gp)
	struct guest_proc *gp;
{
	unsigned char b[2];
	if (copyin((caddr_t)AMIGA_PV_BASE, (caddr_t)b, 2))
		return -1;
	return b[0] & 0x80 ? 0x4000 : 0;
}
/* the master bit back to guest memory; enabling clears the held flag */
void
amiga_pvput(gp, m)
	struct guest_proc *gp;
	int m;
{
	unsigned char b[4];
	b[0] = m ? 0xc0 : 0x40; b[1] = b[2] = b[3] = 0;
	if (copyout((caddr_t)b, (caddr_t)AMIGA_PV_BASE, m ? 4 : 2))
		psignal(curproc, SIGSEGV);
}
/* CIA-A timer B to guest memory, for the patched E-clock read */
void
amiga_pvclock(gp)
	struct guest_proc *gp;
{
	amiga_pvpush(gp);
}
/* bits the guest changed in w since it read p, applied to the device's v */
static unsigned long
amiga_merge(v, p, w)
	unsigned long v, p, w;
{
	return (v & ~(p & ~w)) | (w & ~p);
}
/*
 * The interrupt words from guest memory.  The host only sets INTREQ and
 * ICR bits meanwhile, so the guest's changes since the last read or
 * write are what differs.  Guest context.
 */
void
amiga_pvpull(gp)
	struct guest_proc *gp;
{
	struct amigactr *a = AMIGAP(gp);
	unsigned char b[10];
	int s, i;
	if (a->ac_pvreq > 0xffff || copyin((caddr_t)AMIGA_PV_BASE + 8, (caddr_t)b, 10))
		return;
	s = guest_splhi();
	a->ac_intreq = amiga_merge(a->ac_intreq, a->ac_pvreq, G16(b));
	a->ac_intena = (a->ac_intena & 0x4000) | (G16(b + 2) & 0x3fff);
	a->ac_pvreq = G16(b);
	a->ac_pvena = G16(b + 2);
	for (i = 0; i < 2; i++) {
		a->ac_icr[i] = amiga_merge(a->ac_icr[i], a->ac_pvicr[i], b[4 + i]);
		a->ac_pvicr[i] = b[4 + i];
		a->ac_icrmask[i] = b[8 + i] & 0x7f;
	}
	guest_splx(s);
}
/* INTENAR and INTREQR in the custom chips' page, when they changed */
static void
amiga_cpregs(gp, ena, req)
	struct guest_proc *gp;
	unsigned long ena, req;
{
	struct amigactr *a = AMIGAP(gp);
	unsigned char b[4];
	int i = amiga_slot(gp);

	if (i < 0 || (ena == a->ac_cpena && req == a->ac_cpreq))
		return;
	b[0] = ena >> 8; b[1] = ena; b[2] = req >> 8; b[3] = req;
	if (guest_cpage_set(&amiga_env.ge_sess[i], 0xdff01cUL, b, 4UL) == 0) {
		a->ac_cpena = ena;
		a->ac_cpreq = req;
	}
}
/* the interrupt words and timer B to guest memory, when they changed */
void
amiga_pvpush(gp)
	struct guest_proc *gp;
{
	struct amigactr *a = AMIGAP(gp);
	unsigned long v[5];
	unsigned char b[10];
	int s;
	s = guest_splhi();
	amiga_ipl(a);
	v[0] = a->ac_intreq;
	v[1] = a->ac_intena & 0x3fff;
	v[2] = a->ac_icr[0];
	v[3] = a->ac_icr[1];
	/* with the ICR bits, so an E-clock read sees its wrap */
	v[4] = 0xffff - (a->ac_eclk & 0xffff);
	guest_splx(s);
	if (v[4] != a->ac_pvtb) {
		b[0] = v[4] >> 8; b[1] = v[4];
		if (copyout((caddr_t)b, (caddr_t)AMIGA_PV_BASE + 4, 2) == 0)
			a->ac_pvtb = v[4];
	}
	amiga_cpregs(gp, v[1], v[0]);
	if (v[0] == a->ac_pvreq && v[1] == a->ac_pvena && v[2] == a->ac_pvicr[0] &&
	    v[3] == a->ac_pvicr[1])
		return;
	b[0] = v[0] >> 8; b[1] = v[0]; b[2] = v[1] >> 8; b[3] = v[1]; b[4] = v[2]; b[5] = v[3];
	/* after a reset the timer is stopped and the ICR masks clear */
	b[6] = b[7] = b[8] = b[9] = 0;
	if (copyout((caddr_t)b, (caddr_t)AMIGA_PV_BASE + 8, a->ac_pvreq > 0xffff ? 10 : 6) == 0) {
		a->ac_pvreq = v[0]; a->ac_pvena = v[1];
		a->ac_pvicr[0] = v[2]; a->ac_pvicr[1] = v[3];
	}
}
/* the interrupts and clocks of a machine just reset; under spl */
static void
amiga_intreset(a)
	struct amigactr *a;
{
	a->ac_intena = a->ac_intreq = 0;
	a->ac_icr[0] = a->ac_icr[1] = 0;
	a->ac_icrmask[0] = a->ac_icrmask[1] = 0;
	a->ac_vbl = 0;
	a->ac_bolt = lbolt; a->ac_eclk = 0; a->ac_tarmed = 0;
}
/* guest memory's interrupt words unknown: the next push writes them */
void
amiga_pvreset(a)
	struct amigactr *a;
{
	a->ac_pvreq = a->ac_pvena = a->ac_pvicr[0] = a->ac_pvicr[1] = 0x10000;
}
/* the master bit from guest memory; under spl */
static void
amiga_pvsync(gp, m)
	struct guest_proc *gp;
	int m;
{
	struct amigactr *a = AMIGAP(gp);
	if (m >= 0)
		a->ac_intena = (a->ac_intena & ~0x4000) | m;
}
/* the level ignoring the master bit, which only the guest can read */
static int
amiga_postlevel(gp)
	struct guest_proc *gp;
{
	struct amigactr *a = AMIGAP(gp);
	unsigned short e = a->ac_intena;
	int l;
	a->ac_intena |= 0x4000;
	l = amiga_ipl(a);
	a->ac_intena = e;
	return l;
}
static int
amiga_envlevel(es)
	struct guest_sess *es;
{
	return amiga_postlevel(es->gs_gp);
}
/*
 * Guest context, before an interrupt goes in: the master bit and timer B
 * from guest memory, host socket readiness as PORTS.  Holds the carrier
 * while the guest's own master bit is clear; Enable then sees the flag,
 * writes INTENA, which traps and delivers.
 */
static void amiga_tsync();
static int
amiga_sync(es)
	struct guest_sess *es;
{
	struct guest_proc *gp = es->gs_gp;
	struct proc *p = es->gs_proc;
	int m, s, flag;
	amiga_pvpull(gp);
	m = amiga_pvget(gp);
	s = guest_splhi();
	amiga_pvsync(gp, m);
	if (sigismember(&p->p_sig, SIGPOLL)) {	/* as AMIGAIOC_KICK raises it */
		sigdelset(&p->p_sig, SIGPOLL);
		AMIGAP(gp)->ac_intreq |= 0x0008;
		AMIGAP(gp)->ac_epoch++;
		guest_env_kick(es);
	}
	flag = m == 0 && amiga_postlevel(gp) > GUEST_VIPL(gp) && sigismember(&p->p_sig, AMIGA_SIG);
	guest_splx(s);
	amiga_tsync(gp);
	if (flag) {
		unsigned char b[2];
		b[0] = 0; b[1] = 1;
		copyout((caddr_t)b, (caddr_t)AMIGA_PV_BASE + 2, 2);
	}
	amiga_pvpush(gp);
	amiga_pvclock(gp);
	return m == 0;
}
static int
amiga_ack(es, r, l)
	struct guest_sess *es;
	char *r;
	int l;
{
	AMIGAP(es->gs_gp)->ac_stat.as_intr++;
	return 24 + l;
}
static void
amiga_late_wake(arg)
	caddr_t arg;
{
	int i = (unsigned long *)arg - amiga_answered;
	amiga_late[i] = 1;
	wakeup(arg);
}
/*
 * After the guest rang for a request: wait until the helper has answered
 * it, at most two ticks, so the guest finds the reply without polling.
 */
void
amiga_await(gp)
	struct guest_proc *gp;
{
	int i, s = guest_splhi(), id;
	unsigned long want;
	i = amiga_slot(gp);
	if (i >= 0 && amiga_answers[i]) {
		want = amiga_rung[i];
		amiga_late[i] = 0;
		id = ttimeout(amiga_late_wake, (caddr_t)&amiga_answered[i], 2L);
		while ((long)(amiga_answered[i] - want) < 0 && !amiga_late[i] && amiga_slot(gp) == i)
			/* not interruptible: a held interrupt is a pending signal; the timeout ends it */
			(void)sleep((caddr_t)&amiga_answered[i], PZERO - 1);
		if (id != -1)
			untimeout(id);
	}
	guest_splx(s);
}
/* after the guest enabled interrupts, from its own context */
void
amiga_repost(gp)
	struct guest_proc *gp;
{
	int s = guest_splhi();
	amiga_post(gp);
	guest_splx(s);
}
void
amiga_ring(gp, snd)
	struct guest_proc *gp;
	int snd;
{
	int i, s = guest_splhi();
	unsigned long *r = snd ? amiga_srung : amiga_rung;
	if ((i = amiga_slot(gp)) >= 0) {
		r[i]++;
		wakeup((caddr_t)&r[i]);
	}
	guest_splx(s);
	/* the helpers answer before the guest goes on */
	runrun = 1;
}
/*
 * The interval timer started (d0 E-clock ticks, a0 its latch): due that
 * long after the host clock's last tick.  Guest context.
 */
static void
amiga_tstart(gp, r)
	struct guest_proc *gp;
	char *r;
{
	struct amigactr *a = AMIGAP(gp);
	unsigned long at = GR_A(r, 0);
	int s = guest_splhi();
	a->ac_tdue = a->ac_eclk + (GR_D(r, 0) & 0xffff);
	a->ac_tcia = ((at >> 12) & 15) == 0xd;
	a->ac_tbit = at & 0x200 ? 2 : 1;
	a->ac_tarmed = 1;
	a->ac_tfired = 0;
	guest_splx(s);
}
/* stopped before its interrupt went in: none.  Guest context */
static void
amiga_tsync(gp)
	struct guest_proc *gp;
{
	struct amigactr *a = AMIGAP(gp);
	unsigned char run;
	int s;
	if (copyin((caddr_t)AMIGA_PV_BASE + 14, (caddr_t)&run, 1))
		return;
	s = guest_splhi();
	if (!run) {
		a->ac_tarmed = 0;
		if (a->ac_tfired) {
			a->ac_icr[a->ac_tcia] &= ~a->ac_tbit;
			a->ac_epoch++;
		}
	}
	a->ac_tfired = 0;
	guest_splx(s);
}
/*
 * Each clock tick: VERTB at the frame rate, CIA-A timer B counting the
 * host's E-clock down, the interval timer at its deadline.
 */
static void
amiga_tick(es)
	struct guest_sess *es;
{
	struct amigactr *a = AMIGAP(es->gs_gp);
	unsigned long f, clocks, was = a->ac_eclk;
	long n = lbolt - a->ac_bolt;
	int pal = (a->ac_config.ae_flags & AMIGAF_PAL) != 0;
	if (n <= 0)
		return;
	a->ac_bolt = lbolt;
	f = pal ? 709379 : 715909;
	a->ac_fraction += n % HZ * f;
	clocks = n / HZ * f + a->ac_fraction / HZ;
	a->ac_fraction %= HZ;
	a->ac_eclk += clocks;
	a->ac_vbl += clocks * (pal ? 50 : 60);
	if (a->ac_vbl >= f) {
		a->ac_vbl %= f;
		a->ac_intreq |= 0x0020;
	}
	if ((a->ac_eclk ^ was) >> 16)
		a->ac_icr[0] |= 2;
	if (a->ac_tarmed && (long)(a->ac_eclk - a->ac_tdue) >= 0) {
		a->ac_tarmed = 0;
		a->ac_tfired = 1;
		a->ac_icr[a->ac_tcia] |= a->ac_tbit;
	}
	a->ac_epoch++;
}
static void
amiga_census(a)
	struct amigactr *a;
{
	struct amigacensus *c = &a->ac_census;
	int i;
	printf("amiga census: faults %d priv %d intr %d stop %d last pc %x addr %x\n",
	    (int)a->ac_stat.as_fault, (int)a->ac_stat.as_priv, (int)a->ac_stat.as_intr, (int)a->ac_stat.as_stop,
	    (int)a->ac_stat.as_lastpc, (int)a->ac_stat.as_lastaddr);
	for (i = 0; i < AMIGA_NOTHER && c->priv[i][1]; i++)
		printf("amiga census: privileged pc %x n %d op %x\n", (int)c->priv[i][0],
		    (int)c->priv[i][1], (int)c->priv[i][2]);
	for (i = 0; i < 64 && c->site[i][2]; i++)
		printf("amiga census: site pc %x reg %x n %d\n", (int)c->site[i][0],
		    (int)c->site[i][1], (int)c->site[i][2]);
}
static void
amiga_exit(es)
	struct guest_sess *es;
{
	struct amigactr *a = AMIGAP(es->gs_gp);
	int s, i = es - amiga_env.ge_sess;
	if (a->ac_config.ae_flags & AMIGAF_CENSUS)
		amiga_census(a);
	s = guest_splhi();
	sigdelset(&es->gs_proc->p_sig, AMIGA_SIG);
	amiga_answers[i] = 0;
	wakeup((caddr_t)&amiga_rung[i]);
	wakeup((caddr_t)&amiga_srung[i]);
	wakeup((caddr_t)&amiga_answered[i]);
	guest_splx(s);
}
static int
amiga_refl(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	return guest_reflcpu(gp, r, GR_PC(r), (GR_FV(r) & 0xf000) | (v << 2), (char *)0);
}
/* reset: the interrupt state and the pv page as at power-on */
static void
amiga_reset(gp, r)
	struct guest_proc *gp;
	char *r;
{
	struct amigactr *a = AMIGAP(gp);
	int s;

	amiga_pvput(gp, 0);
	a->ac_pvtb = 0x10000;
	s = guest_splhi();
	amiga_intreset(a);
	a->ac_epoch++;
	gp->gp_vpend = 0; a->ac_fraction = 0;
	sigdelset(&gp->gp_proc->p_sig, AMIGA_SIG);
	guest_splx(s);
	amiga_pvreset(a);
	amiga_pvpush(gp);
}
/* stop: the pv page's interrupt state first, then the session's wait */
static void
amiga_stop(gp, r)
	struct guest_proc *gp;
	char *r;
{
	struct amigactr *a = AMIGAP(gp);
	int c, s;

	a->ac_stat.as_stop++;
	amiga_pvpull(gp);
	c = amiga_pvget(gp);
	s = guest_splhi();
	amiga_pvsync(gp, c);
	guest_splx(s);
	if ((c = amiga_slot(gp)) < 0) {
		guest_trapret();
		return;
	}
	amiga_env.ge_sess[c].gs_halt = GH_IPL;
	guest_env_after(&amiga_env.ge_sess[c]);
	amiga_pvclock(gp);
}
static int
amiga_priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	struct amigactr *a = AMIGAP(gp);
	int e, c;
	if (v == 8 && (gp->gp_vsr & 0x2000)) {
		a->ac_stat.as_priv++;
		a->ac_stat.as_lastpc = GR_PC(r);
		if (a->ac_config.ae_flags & AMIGAF_CENSUS) {
			struct amigacensus *cc = &a->ac_census;
			for (c = 0; c < AMIGA_NOTHER && cc->priv[c][1]; c++)
				if (cc->priv[c][0] == GR_PC(r)) break;
			if (c < AMIGA_NOTHER) {
				cc->priv[c][0] = GR_PC(r);
				cc->priv[c][2] = fuword((caddr_t)GR_PC(r)) >> 16 & 0xffff;
				cc->priv[c][1]++;
			}
		}
	}
	if ((e = guest_privtrap(gp, r, v)) <= 1)
		return e;
	return amiga_refl(gp, r, e);
}
/* the boot extension's host calls */
static int
amiga_hostcall(es, r, n)
	struct guest_sess *es;
	char *r;
	int n;
{
	/* the interrupt words changed, or Enable found an interrupt held */
	if (n == 3) {
		struct guest_proc *gp = es->gs_gp;
		int m, s;
		amiga_pvpull(gp);
		if ((m = amiga_pvget(gp)) > 0) {
			s = guest_splhi();
			amiga_pvsync(gp, m);
			guest_splx(s);
			amiga_pvput(gp, m);
		}
		amiga_repost(gp);
		return 0;
	}
	if (n == 4) {
		amiga_tstart(es->gs_gp, r);
		return 0;
	}
	amiga_ring(es->gs_gp, n == 2);
	if (n == 0)
		amiga_await(es->gs_gp);
	return 0;
}
static int
amiga_enter(es, arg)
	struct guest_sess *es;
	caddr_t arg;
{
	struct amigaenter ae;
	struct guest_proc *gp = es->gs_gp;
	struct amigactr *a = AMIGAP(gp);
	int i = es - amiga_env.ge_sess, e;
	if (copyin(arg, (caddr_t)&ae, sizeof ae)) return EFAULT;
	if (ae.ae_version != AMIGA_ABI_VERSION || ae.ae_chipsize != AMIGA_CHIP_SIZE ||
	    ae.ae_fastsize > AMIGA_FAST_MAX || (ae.ae_fastsize & 0xfffff) ||
	    (ae.ae_flags & ~(AMIGAF_PAL | AMIGAF_CENSUS | AMIGAF_PVALL | AMIGAF_SCSI)) ||
	    (ae.ae_flags & AMIGAF_PVALL) != AMIGAF_PVALL) return EINVAL;
	/* the census goes to the console */
	if ((ae.ae_flags & AMIGAF_CENSUS) && !suser(curproc->p_cred)) return EPERM;
	a->ac_gp = gp; a->ac_config = ae;
	a->ac_pvtb = 0x10000;
	amiga_pvreset(a);
	a->ac_stat.as_version = AMIGA_ABI_VERSION;
	a->ac_stat.as_pid = curproc->p_pid;
	amiga_intreset(a);
	if ((e = amiga_cpages(es)) != 0)
		return e;
	/* known words in guest memory, so the guest's first changes are seen */
	amiga_pvpush(gp);
	dlm_cacheflush();
	amiga_heard[i] = amiga_rung[i];
	amiga_answered[i] = amiga_rung[i];
	amiga_answers[i] = 0;
	return 0;
}
static int
amiga_ioctl(env, es, cmd, arg, mode, rvp)
	struct guest_env *env;
	struct guest_sess *es;
	int cmd, mode;
	caddr_t arg;
	int *rvp;
{
	struct amigainfo ai;
	struct guest_proc *gp;
	int e, i, s;
	if (cmd == AMIGAIOC_SNDWAIT) {
		s = guest_splhi();
		i = amiga_pidslot((long)arg);
		e = i >= 0 ? 0 : ESRCH;
		if (!e) {
			if (amiga_srung[i] == amiga_sheard[i] &&
			    sleep((caddr_t)&amiga_srung[i], (PZERO + 1) | PCATCH))
				e = EINTR;
			else
				amiga_sheard[i] = amiga_srung[i];
		}
		guest_splx(s);
		return e;
	}
	if (cmd == AMIGAIOC_WAITN || cmd == AMIGAIOC_GSTAT) {
		struct amigawait aw;
		struct amigastat st;
		if (copyin(arg, (caddr_t)&aw, sizeof aw)) return EFAULT;
		s = guest_splhi();
		i = amiga_pidslot((long)aw.aw_pid);
		e = i >= 0 ? 0 : ESRCH;
		if (!e && cmd == AMIGAIOC_GSTAT)
			st = AMIGAP(amiga_env.ge_sess[i].gs_gp)->ac_stat;
		else if (!e) {
			if (aw.aw_flags & AMIGAW_ANSWER) {
				amiga_answers[i] = 1;
				amiga_answered[i] = aw.aw_heard;
				wakeup((caddr_t)&amiga_answered[i]);
			}
			if (amiga_rung[i] == aw.aw_heard &&
			    sleep((caddr_t)&amiga_rung[i], (PZERO + 1) | PCATCH))
				e = EINTR;
			aw.aw_heard = amiga_rung[i];
		}
		guest_splx(s);
		if (e) return e;
		if (cmd == AMIGAIOC_GSTAT)
			return copyout((caddr_t)&st, (caddr_t)aw.aw_stat, sizeof st) ? EFAULT : 0;
		return copyout((caddr_t)&aw, arg, sizeof aw) ? EFAULT : 0;
	}
	if (cmd == AMIGAIOC_KICK || cmd == AMIGAIOC_WAIT) {
		s = guest_splhi();
		i = amiga_pidslot((long)arg);
		e = i >= 0 ? 0 : ESRCH;
		if (!e && cmd == AMIGAIOC_KICK) {
			/* a helper's doorbell: PORTS, as an expansion board raises it */
			gp = amiga_env.ge_sess[i].gs_gp;
			AMIGAP(gp)->ac_intreq |= 0x0008;
			AMIGAP(gp)->ac_epoch++;
			guest_env_kick(&amiga_env.ge_sess[i]);
		} else if (!e) {
			/* the slot may be reused while asleep: the caller asks again */
			if (amiga_rung[i] == amiga_heard[i] &&
			    sleep((caddr_t)&amiga_rung[i], (PZERO + 1) | PCATCH))
				e = EINTR;
			else
				amiga_heard[i] = amiga_rung[i];
			amiga_sheard[i] = amiga_srung[i];
			amiga_answered[i] = amiga_rung[i];
			amiga_answers[i] = 0;
		}
		guest_splx(s);
		return e;
	}
	if (cmd == AMIGAIOC_INFO) {
		ai.ai_version = AMIGA_ABI_VERSION;
		ai.ai_features = AMIGA_FEAT_BASE | AMIGA_FEAT_KICK | AMIGA_FEAT_SNDBELL |
		    AMIGA_FEAT_HOSTPC;
		if (amiga_cpup && (*amiga_cpup == 40 || *amiga_cpup == 60)) ai.ai_features |= AMIGA_FEAT_EXPERIMENTAL;
		return copyout((caddr_t)&ai, arg, sizeof ai) ? EFAULT : 0;
	}
	gp = GUESTP(curproc);
	if (!gp || gp->gp_prof != &amiga_env.ge_prof) return ENXIO;
	if (cmd == AMIGAIOC_LEAVE) return guest_detach(&amiga_env.ge_prof);
	if (cmd == AMIGAIOC_SOCK) return amiga_sock(arg);
	if (cmd == AMIGAIOC_HALT) return guest_halt();
	if (cmd == AMIGAIOC_HOSTPC) {
		struct amigahostpc h;
		if (copyin(arg, (caddr_t)&h, sizeof h)) return EFAULT;
		if (h.ah_hi < h.ah_lo) return EINVAL;
		gp->gp_hostlo = h.ah_lo;
		gp->gp_hosthi = h.ah_hi;
		return 0;
	}
	if (cmd == AMIGAIOC_STAT)
		return copyout((caddr_t)&AMIGAP(gp)->ac_stat, arg, sizeof(struct amigastat)) ? EFAULT : 0;
	return EINVAL;
}
int
amigaopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	return guest_env_open(&amiga_env, devp, flag, otyp, cr);
}
int
amigaclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	return guest_env_close(&amiga_env, dev, flag, otyp, cr);
}
int
amigaioctl(dev, cmd, arg, mode, cr, rvp)
	dev_t dev;
	int cmd, mode;
	caddr_t arg;
	struct cred *cr;
	int *rvp;
{
	if (cmd == AMIGAIOC_MAPROM)
		return GUESTP(curproc) && GUESTP(curproc)->gp_prof != &amiga_env.ge_prof ?
		    EPERM : amiga_maprom(dev, rvp);
	return guest_env_ioctl(&amiga_env, dev, cmd, arg, mode, cr, rvp);
}
static int
amigaguest_load()
{
	int v;
	struct guest_disp *d;
	amiga_env.ge_name = "amiga";
	amiga_env.ge_wrapper = &amigaguest_wrapper;
	amiga_env.ge_major = AMIGA_MAJOR;
	amiga_env.ge_entercmd = AMIGAIOC_ENTER;
	amiga_env.ge_privsz = sizeof(struct amigactr);
	amiga_env.ge_gpflags = GPF_PRIV | GPF_FTRAP | GPF_FTRAP13;
	amiga_env.ge_prof.gpf_reset = amiga_reset;
	amiga_env.ge_prof.gpf_stop = amiga_stop;
	amiga_env.ge_prof.gpf_privf = GPR_ISPILL;
	amiga_env.ge_nsess = AMIGA_NGUEST;
	amiga_env.ge_sig = AMIGA_SIG;
	amiga_env.ge_flags = GEF_SIGTAIL | GEF_ONSTACK;
	amiga_env.ge_enter = amiga_enter;
	amiga_env.ge_exit = amiga_exit;
	amiga_env.ge_level = amiga_envlevel;
	amiga_env.ge_sync = amiga_sync;
	amiga_env.ge_ack = amiga_ack;
	amiga_env.ge_tick = amiga_tick;
	amiga_env.ge_ioctl = amiga_ioctl;
	amiga_env.ge_access = amiga_access;
	amiga_env.ge_hostcall = amiga_hostcall;
	amiga_env.ge_hostva = AMIGA_HOSTVA;
	amiga_env.ge_hostn = 5;
	amiga_env.ge_hoststep = 8;
	amiga_env.ge_hostvec = 4;
	amiga_env.ge_hostlen = 2;
	for (v = 2; v < 48; v++) {
		if ((v > 11 && v < 32) || v == 32) continue;
		d = &amiga_env.ge_prof.gpf_disp[v];
		d->gd_flags = GDF_USER;
		d->gd_kind = GD_EMULATE;
		d->gd_fn = v == 2 ? amiga_fault : v == 8 || v == 11 ? amiga_priv : amiga_refl;
	}
	return guest_env_add(&amiga_env);
}
static int
amigaguest_unload()
{
	return guest_env_del(&amiga_env);
}
struct mod_drv_data amigaguest_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	  { amigaopen, amigaclose, nodev, nodev, amigaioctl, nodev, nodev, nodev,
	    nodev, nodev, 0, 0, amiga_devflag }, AMIGA_MAJOR, 1 }
};
MOD_DRV_WRAPPER(amigaguest, amigaguest_load, amigaguest_unload, 0, "Amiga guest");
