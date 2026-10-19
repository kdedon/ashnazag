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
#include "sys/vnode.h"
#include "sys/file.h"
#include "sys/mman.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "vm/hat.h"
#include "vm/seg_dev.h"
#include "vm/page.h"
#include "guest_mem.h"
#include "guest_lone.h"
#include "hsock.h"

extern int nodev(), ttimeout(), untimeout();
extern int runrun;
extern int fpu_present;
extern void dlm_cacheflush();
extern long lbolt;
extern struct modwrapper tosguest_wrapper;
extern struct proc *prfind();

struct tosctr tosc;
int	tos_trace = 0;		/* 1: console lines for bus errors and odd accesses */

static struct guest_profile tos_profile;
static int tos_nopen;
static struct proc *tos_stproc;	/* allocated the ST-RAM block; only it maps it */
static pid_t tos_stpid;
static struct guest_ram *tos_ram;
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
 * Something may want the guest.  A masked request waits for the IPL to
 * drop (gp_vpend sends that through the trap tail); a sleeping guest is
 * woken; a running one gets the carrier.  rr: also reschedule at once.
 */
void
tos_kick(rr)
	int rr;
{
	struct tosctr *t = &tosc;
	int l;

	if (t->t_state != 1 || t->t_paused)
		return;
	l = tos_level();
	t->t_gp->gp_vpend = l << 8;
	if (l <= vipl(t->t_gp))
		return;
	if (t->t_sleeping)
		wakeup((caddr_t)&t->t_sleeping);
	else {
		psignal(t->t_proc, TOS_SIG);
		if (rr)
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
	gp->gp_flags |= GPF_IDROP;	/* the CPU takes the interrupt that ends stop */
	t->t_st.ts_slept += lbolt - t0;
	splx_(s);
}

/* ---- delivery ---- */

/*
 * 1: hold the requests at an emulation tail, so they are taken where
 * the guest runs its own code (the tick, a system call's return), not
 * mostly on the few instructions that trap.  An IPL just dropped takes
 * them at once, as on the CPU; so does a wait past a whole tick.
 */
static int
tos_hold(gp)
	struct guest_proc *gp;
{
	struct tosctr *t = &tosc;

	if (!(gp->gp_flags & GPF_ETAIL) || (gp->gp_flags & GPF_IDROP) || t->t_paused)
		return 0;
	if (!t->t_held) {
		t->t_held = 1;
		t->t_heldsince = lbolt;
		t->t_st.ts_held++;
	}
	return lbolt - t->t_heldsince < 2;
}

/*
 * Every interrupt the IPL lets through, each frame on top of the last
 * as the CPU takes them; then the carrier is no longer needed.
 */
static void
tos_intr(gp, r)
	struct guest_proc *gp;
	char *r;
{
	struct tosctr *t = &tosc;
	int s, l, vec;

	/* a host call finishes first, in our code or the C library on our stack */
	if (t->t_gp != gp || (u_long)GR_PC(r) - gp->gp_hostlo < gp->gp_hosthi - gp->gp_hostlo ||
	    (u_long)GR_USP(r) - t->t_stklo < t->t_stkhi - t->t_stklo)
		return;
	/*
	 * A fault frame resumes its instruction part way through; restarting
	 * it would repeat its side effects.  The CPU takes interrupts between
	 * instructions, so these wait for the next tick, unless the handler
	 * finished the instruction (its frame goes).
	 */
	switch (GR_FV(r) >> 12) {
	case 9: case 0xa: case 0xb:
		if (!(u.u_sigflag & USTKCLEAR))
			return;
	}
	s = splhi_();
	while (!t->t_paused && (l = tos_level()) > vipl(gp)) {
		/* the tick posts the carrier again; meanwhile the gate's fast paths stay open */
		if (tos_hold(gp)) {
			sigdelset(&gp->gp_proc->p_sig, TOS_SIG);
			break;
		}
		t->t_held = 0;
		splx_(s);
		tos_unsink();
		s = splhi_();
		if (l == 6) {
			vec = mfp_ack();
			splx_(s);
		} else {
			if (t->t_vblowed)
				t->t_vblowed--;
			else
				t->t_vblpend = 0;
			t->t_st.ts_vbl++;
			vec = 28;
			splx_(s);
		}
		if (guest_reflect(gp, r, GR_PC(r), vec << 2, (char *)0, 2, l) < 0) {
			s = splhi_();
			if (l == 6)
				mfp_unack(vec);		/* still pending for a debugger */
			else
				t->t_vblpend = 1;
			splx_(s);
			printf("tos: interrupt with no guest stack, pc %x\n", (int)GR_PC(r));
			psignal(curproc, SIGSEGV);
			return;
		}
		if (GR_FV(r) >> 12)
			u.u_sigflag |= USTKCLEAR;
		s = splhi_();
	}
	l = tos_level();
	gp->gp_vpend = l << 8;
	if (!t->t_paused && l <= vipl(gp)) {
		t->t_held = 0;
		sigdelset(&gp->gp_proc->p_sig, TOS_SIG);
	}
	splx_(s);
}

/* the carrier waits while the guest's IPL masks every request */
static int
tos_fsig(p, gp)
	struct proc *p;
	struct guest_proc *gp;
{
	k_sigset_t h;
	int s, n;

	if (gp->gp_flags & TGF_SOLO)
		return __amix_fsig(p);
	s = splhi_();
	h = p->p_hold;
	sigdelset(&p->p_hold, TOS_SIG);
	if (tosc.t_gp != gp || (!tosc.t_paused && (tos_level() <= vipl(gp) || tos_hold(gp))))
		sigaddset(&p->p_hold, TOS_SIG);
	n = __amix_fsig(p);
	p->p_hold = h;
	splx_(s);
	return n;
}

/* the process that paused the guest still runs */
static int
tos_pauser()
{
	struct proc *p = prfind(tosc.t_ppid);

	return p && p == tosc.t_pproc && p->p_stat != SZOMB;
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
	int s, l, id;

	if (sig != TOS_SIG || (gp->gp_flags & TGF_SOLO))
		return __amix_sendsig(sig, sip, hdlr);
	if (tosc.t_gp != gp)
		return 1;
	s = splhi_();
	while (tosc.t_paused && tosc.t_gp == gp) {
		if (!tos_pauser()) {
			tosc.t_paused = 0;
			break;
		}
		id = ttimeout(wakeup, (caddr_t)&tosc.t_paused, (long)HZ);
		l = sleep((caddr_t)&tosc.t_paused, (PZERO + 1) | PCATCH);
		if (id != -1)
			untimeout(id);
		if (l)
			break;
	}
	splx_(s);
	tos_intr(gp, r);
	return 1;
}

/* ---- dispositions ---- */

/* the session's counters; a lone guest's go to a scratch copy */
static struct tosstat solost;
#define	TST(gp)	((gp)->gp_flags & TGF_SOLO ? &solost : &tosc.t_st)

static int tos_refl0();

/* the CPU's own frame, through the guest's vector */
static int
tos_refl(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int n;

	if (gp->gp_flags & TGF_SOLO)
		return guest_lone_trap(gp, r, v);
	switch (GR_FV(r) >> 12) {
	case 0: n = 0; break;
	case 2: case 3: n = 4; break;
	case 4: n = 8; break;
	default: return 1;
	}
	TST(gp)->ts_lastpc = GR_PC(r);
	if (v < 64)
		TST(gp)->ts_refl[v]++;
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
		TST(gp)->ts_refl[v]++;
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

static int tos_fline();

/* vector 8: user mode reflects; reset is a no-op; the rest guestcore */
static int
tos_priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	char sr[2];
	int op;

	if (gp->gp_flags & TGF_SOLO)
		return guest_lone_trap(gp, r, v);

	TST(gp)->ts_lastpc = GR_PC(r);
	if (!(gp->gp_vsr & SR_S))
		return tos_refl(gp, r, v);
	op = opword(r);
	if ((op & 0xffc0) == 0xf000)	/* a 68030's MMU instructions */
		return tos_fline(gp, r, v);
	if (!fpu_present && (op & 0xfe00) == 0xf200 && (op >> 7 & 3) == 2)
		return tos_refl0(gp, r, 11, GR_PC(r));	/* fsave, frestore: no FPU */
	TST(gp)->ts_priv++;
	if (op == 0x4e70) {		/* reset */
		GR_PC(r) += 2;
		guest_trapret();
		return 0;
	}
	if (op == 0x4e72 && copyin((caddr_t)GR_PC(r) + 2, sr, 2) == 0) {
		guest_setsr(gp, r, (int)G16(sr));
		GR_PC(r) += 4;
		if (!(gp->gp_flags & TGF_SOLO))
			tos_stop_insn(gp);
		guest_trapret();
		return 0;
	}
	if (op >> 8 == 0xf4 || (op == 0x4e7b &&
	    copyin((caddr_t)GR_PC(r) + 2, sr, 2) == 0 && (G16(sr) & 0xfff) == 2))
		TST(gp)->ts_cache++;	/* cinv, cpush, CACR: pushed for real */
	if (guest_priv(gp, r, v) == 0)
		return 0;
	return tos_refl0(gp, r, 4, GR_PC(r));	/* illegal instruction */
}

/* vector 11: 68030 MMU instructions to guestcore; FPU to the host */
static int
tos_fline(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int op = opword(r);

	if (gp->gp_flags & TGF_SOLO)
		return guest_lone_trap(gp, r, v);

	TST(gp)->ts_lastpc = GR_PC(r);
	if ((op & 0xffc0) == 0xf000) {
		if (!(gp->gp_vsr & SR_S))
			return tos_refl0(gp, r, 8, GR_PC(r));
		if (guest_fline(gp, r, v) == 0) {
			tosc.t_st.ts_mmu++;
			return 0;
		}
	}
	if (((op >> 9) & 7) == 1 && fpu_present)
		return 1;
	return tos_refl(gp, r, v);
}

/* trap #0: a host system call from the machine layer; anyone else's goes through the vector */
static int
tos_sys(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	if (gp->gp_flags & TGF_SOLO)
		return guest_lone_trap(gp, r, v);

	/* host system calls: the cartridge's, and a host call's on our stack */
	if (((unsigned long)GR_PC(r) < 0xfa0000 || (unsigned long)GR_PC(r) >= 0xfc0000) &&
	    (unsigned long)GR_USP(r) - tosc.t_stklo >= tosc.t_stkhi - tosc.t_stklo)
		return tos_refl(gp, r, v);
	TST(gp)->ts_sys++;
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
	if (t->t_vblpend && curproc == t->t_proc && vipl(t->t_gp) >= 4)
		t->t_st.ts_vblheld++;
	else if (t->t_vblpend && t->t_held && t->t_vblowed < 2)
		t->t_vblowed++;		/* goes out after the held one */
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
	if (pg->gp_flags & TGF_SOLO)
		return 0;
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
	guest_ram_free(tos_ram);
	tos_ram = 0;
	tos_stproc = 0;
	return 0;
}

/* the passthrough ST-RAM block */
int
tosmmap(dev, off, prot)
	dev_t dev;
	off_t off;
	int prot;
{
	return off < 0 ? -1 : guest_ram_frame(tos_ram, (unsigned long)off);
}

int
tossegmap(dev, off, as, addrp, len, prot, maxprot, flags, cr)
dev_t dev;
off_t off;
struct as *as;
addr_t *addrp;
u_int len, prot, maxprot, flags;
struct cred *cr;
{
	if (curproc != tos_stproc || curproc->p_pid != tos_stpid)
		return EACCES;
	return guest_ram_map(tos_ram, dev, off, as, addrp, len, prot, maxprot, flags);
}

static int
stram(size, flags)
unsigned long size;
int flags;
{
	int e;

	if (GUESTP(curproc))
		return EPERM;
	if (tosc.t_state != 0)
		return EBUSY;
	/* a session's mappings hold their own reference */
	if (tos_ram)
		guest_ram_free(tos_ram);
	tos_ram = 0;
	if ((e = guest_ram_new(size, flags, 0xE00000UL, &tos_ram)) == 0) {
		tos_stproc = curproc;
		tos_stpid = curproc->p_pid;
	}
	return e;
}

int
tosrdwr(dev, uiop, cr)
	dev_t dev;
	struct uio *uiop;
	struct cred *cr;
{
	return EINVAL;
}

static int solo();

extern void hat_memload(), flushmmu();
extern page_t *page_numtookpp();
extern void ppcopy();
__asm__(".weak hat_cm_ram");
extern long hat_cm_ram;
static long *__volatile__ tos_hat4k = &hat_cm_ram;

#define	IOPG	0x1000
#define	SINK	3		/* the RAM object's page after the three I/O pages */

/*
 * Map the RAM object's page at off to the I/O page at va.  The last page
 * of the address space has no segment (none can end at 2^32): its PTEs
 * go under the $FFF000 alias's.  0, or -1 if it is not mapped.
 */
static int
ioload(va, off, prot)
	unsigned long va, off;
	u_int prot;
{
	struct seg *seg;
	long pf;
	page_t *pp;
	int step = tos_hat4k ? 4096 : 2048, n;

	if (curproc != tos_stproc)
		return -1;
	seg = as_segat(curproc->p_as, (caddr_t)(va >= 0xfffff000 ? 0xfff000 : va));
	if (!seg || (va >= 0xfffff000 && seg->s_base != (addr_t)0xfff000))
		return -1;
	for (n = 0; n < IOPG; n += step) {
		if ((pf = guest_ram_frame(tos_ram, off + n)) < 0 || !(pp = page_numtookpp((u_int)pf)))
			return -1;
		hat_memload(seg, (addr_t)(va + n), pp, prot | PROT_USER, HAT_NOFLAGS);
		flushmmu((addr_t)(va + n), 1);	/* the ATC may hold the old protection */
	}
	return 0;
}

int
tos_toppage()
{
	return ioload(0xfffff000, tosc.t_ramsize + 2 * IOPG, PROT_READ);
}

/* the RAM object's offset of the I/O page holding a, or 0 */
static unsigned long
iooff(a)
	unsigned long a;
{
	static unsigned long pg[3] = { 0xff8000, 0xff9000, 0xfff000 };
	int i;

	if (a >> 24 != 0 && a >> 24 != 0xff)
		return 0;
	for (i = 0; i < 3; i++)
		if ((a & 0xfff000) == pg[i])
			return tosc.t_ramsize + i * IOPG;
	return 0;
}

/* the RAM object's I/O page at off copied to the one at to */
static int
iocopy(off, to)
	unsigned long off, to;
{
	long f, k;
	page_t *a, *b;
	int step = tos_hat4k ? 4096 : 2048, n;

	for (n = 0; n < IOPG; n += step) {
		if ((f = guest_ram_frame(tos_ram, off + n)) < 0 || (k = guest_ram_frame(tos_ram, to + n)) < 0 ||
		    !(a = page_numtookpp((u_int)f)) || !(b = page_numtookpp((u_int)k)))
			return -1;
		ppcopy(a, b);
	}
	return 0;
}

/*
 * A write to an I/O page: the page is saved to the sink and made writable;
 * tos_unsink puts the saved constants back.  The mapping keeps its page:
 * the 68030's HAT refuses to change one in place.
 */
int
tos_iowrite(a)
	unsigned long a;
{
	struct tosctr *t = &tosc;
	unsigned long off = iooff(a);

	if (off == 0)
		return -1;
	tos_unsink();
	if (iocopy(off, t->t_ramsize + SINK * IOPG) < 0 ||
	    ioload(a & ~(IOPG - 1), off, PROT_READ | PROT_WRITE) < 0)
		return -1;
	t->t_sinkva = a & ~(IOPG - 1);
	t->t_sinkoff = off;
	return 0;
}

/* the I/O page's constants back, read-only */
void
tos_unsink()
{
	struct tosctr *t = &tosc;

	if (t->t_sinkva && t->t_gp && GUESTP(curproc) == t->t_gp) {
		(void)iocopy(t->t_ramsize + SINK * IOPG, t->t_sinkoff);
		dlm_cacheflush();	/* the 68030's data cache holds the guest's writes */
		(void)ioload(t->t_sinkva, t->t_sinkoff, PROT_READ);
		t->t_sinkva = 0;
	}
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
	if (te.te_flags & TEF_NOMACH)
		return solo();
	if (GUESTP(curproc))
		return EBUSY;
	s = splhi_();
	if (t->t_state != 0) {
		splx_(s);
		return EBUSY;
	}
	if (curproc != tos_stproc || curproc->p_pid != tos_stpid) {
		splx_(s);
		return EACCES;		/* only the RAM's owner maps the I/O pages */
	}
	t->t_state = 2;			/* claimed: the tick and hooks wait for 1 */
	splx_(s);
	if ((e = guest_attach(&tos_profile)) != 0) {
		t->t_state = 0;
		return e;
	}
	gp = GUESTP(curproc);
	gp->gp_flags |= GPF_PRIV | GPF_FTRAP | GPF_CPU030;
	gp->gp_vsr = 0x2700;
	gp->gp_vpend = 0;
	gp->gp_vusp = gp->gp_vvbr = gp->gp_vcacr = 0;
	gp->gp_hostlo = te.te_hostlo;
	gp->gp_hosthi = te.te_hosthi;
	bzero((caddr_t)&t->t_proc, sizeof *t - ((caddr_t)&t->t_proc - (caddr_t)t));
	t->t_proc = curproc;
	t->t_gp = gp;
	t->t_uid = cr->cr_uid;
	t->t_ramsize = te.te_ramsize;
	t->t_flags = te.te_flags;
	t->t_stklo = te.te_stklo;
	t->t_stkhi = te.te_stkhi;
	t->t_ien = 0xffff;
	t->t_st.ts_pid = curproc->p_pid;
	tos_devinit(t);
	(void)tos_toppage();
	dlm_cacheflush();		/* the launcher's ROM and cartridge stores reach memory */
	t->t_state = 1;
	if ((t->t_tid = ttimeout(tos_tick, (caddr_t)0, 1L)) == -1)
		t->t_tid = 0;
	return 0;
}

/* a guest of its own: no machine, no tick, any number of them */
static int
solo()
{
	int e = guest_lone_enter(&tos_profile);
	if (!e)
		GUESTP(curproc)->gp_flags |= TGF_SOLO;
	return e;
}

/* a socket call of the container's (struct tossock); it may block as the descriptor says */
static int
tsock(arg)
	caddr_t arg;
{
	struct tossock so;
	struct hs h;
	char a[HS_ADDR], o[64];
	int e, v = 0, n = HS_ADDR, sc = u.u_syscall;

	if (copyin(arg, (caddr_t)&so, sizeof so))
		return EFAULT;
	if (so.so_alen < 0 || so.so_alen > sizeof so.so_addr)
		return EINVAL;
	bcopy(so.so_addr, a, sizeof so.so_addr);
	if (so.so_op == TSO_SOCKET)
		e = hs_socket(so.so_gap, 2, (int)so.so_arg, 0, &v);
	else if ((e = hs_attach(&h, (int)so.so_fd, so.so_gap)) == 0) {
		switch (so.so_op) {
		case TSO_BIND:
			e = hs_bind(&h, a, (int)so.so_alen);
			break;
		case TSO_CONNECT:
			e = hs_connect(&h, a, (int)so.so_alen);
			break;
		case TSO_LISTEN:
			e = hs_listen(&h, (int)so.so_arg);
			break;
		case TSO_ACCEPT:
			e = hs_accept(&h, &v, a, &n);
			break;
		case TSO_SEND:
			e = hs_send(&h, so.so_buf, (int)so.so_len, (int)so.so_arg,
			    so.so_alen ? a : (char *)0, (int)so.so_alen, &v);
			break;
		case TSO_RECV:
			e = hs_recv(&h, so.so_buf, (int)so.so_len, (int)so.so_arg, a, &n, &v);
			break;
		case TSO_CONNWAIT:
			v = hs_connwait(&h);
			break;
		case TSO_NAME:
			e = hs_name(&h, (int)so.so_arg, a, &n);
			break;
		case TSO_GETOPT:
			if ((v = (int)so.so_len) < 0)
				e = EINVAL;
			else if (v > sizeof o)
				v = sizeof o;
			if (e == 0 && (e = hs_getopt(&h, (int)(so.so_arg >> 16) & 0xffff,
			    (int)so.so_arg & 0xffff, o, &v)) == 0 && copyout(o, so.so_buf, v))
				e = EFAULT;
			break;
		case TSO_SETOPT:
			if (so.so_len < 0 || so.so_len > sizeof o)
				e = EINVAL;
			else if (copyin(so.so_buf, o, (int)so.so_len))
				e = EFAULT;
			else
				e = hs_setopt(&h, (int)(so.so_arg >> 16) & 0xffff,
				    (int)so.so_arg & 0xffff, o, (int)so.so_len);
			break;
		case TSO_SHUTDOWN:
			e = hs_shutdown(&h, (int)so.so_arg);
			break;
		default:
			e = EINVAL;
		}
	}
	u.u_syscall = sc;
	if (e)
		return e;
	so.so_rv = v;
	bcopy(a, so.so_addr, sizeof so.so_addr);
	return copyout((caddr_t)&so, arg, sizeof so) ? EFAULT : 0;
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
	struct tosvideo tv;
	int s;

	if (cmd == TOSIOC_ENTER)
		return enter(arg, cr);
	if (cmd == TOSIOC_STRAM)
		return stram((unsigned long)arg, GMR_CONTIG);
	if (cmd == TOSIOC_RAM)
		return stram((unsigned long)arg, 0);
	if (cmd == TOSIOC_SOCK && GUESTP(curproc) &&
	    GUESTP(curproc)->gp_prof == &tos_profile && (GUESTP(curproc)->gp_flags & TGF_SOLO))
		return tsock(arg);
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
	case TOSIOC_VIDEO:
		s = splhi_();
		tv.tv_vbl = t->t_st.ts_vbl;
		tv.tv_vblheld = t->t_st.ts_vblheld;
		splx_(s);
		return copyout((caddr_t)&tv, arg, sizeof tv) ? EFAULT : 0;
	case TOSIOC_PAUSE:
		s = splhi_();
		t->t_paused = arg != 0;
		t->t_ppid = curproc->p_pid;
		t->t_pproc = curproc;
		if (t->t_paused)
			psignal(t->t_proc, TOS_SIG);
		else
			wakeup((caddr_t)&t->t_paused);
		splx_(s);
		tos_kick(1);
		return 0;
	case TOSIOC_STAT:
		t->t_st.ts_fpriv = guest_nfpriv;
		t->t_st.ts_ftrap = guest_nftrap;
		t->t_st.ts_linea = guest_nlinea;
		return copyout((caddr_t)&t->t_st, arg, sizeof t->t_st) ? EFAULT : 0;
	case TOSIOC_SND: {
		struct tossndio sn;

		if (copyin(arg, (caddr_t)&sn, sizeof sn))
			return EFAULT;
		tos_snd(&sn);
		return copyout((caddr_t)&sn, arg, sizeof sn) ? EFAULT : 0;
	}
	case TOSIOC_SOCK:
		return curproc == t->t_proc ? tsock(arg) : EPERM;
	case TOSIOC_TIMER:
		if (curproc != t->t_proc)
			return EPERM;
		tos_xbtimer((unsigned long)arg);
		tos_kick(0);
		return 0;
	case TOSIOC_INTEN:
		if (curproc != t->t_proc)
			return EPERM;
		tos_inten((unsigned long)arg);
		return 0;
	case TOSIOC_HALT:
		return curproc == t->t_proc ? guest_halt() : EPERM;
	}
	return EINVAL;
}

/* vector 61: movep on a 68060, its accesses resolved as tos_fault's */
int
tos_isp(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned long fa;
	int ssw, e, n;

	for (n = 0; n < 4; n++) {
		if ((e = guest_movep(gp, r, &fa, &ssw)) == -1)
			return 1;	/* the host's: 64-bit multiplies */
		if (e == 0) {
			TST(gp)->ts_isp++;
			guest_trapret();
			return 0;
		}
		if ((gp->gp_flags & TGF_SOLO) || (e = tos_access(gp, r, fa, ssw)) == 1)
			break;
		if (e == 2)
			return 0;
	}
	psignal(curproc, SIGSEGV);
	guest_trapret();
	return 0;
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
	tos_profile.gpf_intr = tos_intr;
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
	d = &tos_profile.gpf_disp[61];
	d->gd_flags = GDF_USER;
	d->gd_kind = GD_EMULATE;
	d->gd_fn = tos_isp;
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
	  { tosopen, tosclose, tosrdwr, tosrdwr, tosioctl, tosmmap, tossegmap, nodev,
	    nodev, nodev, 0, 0, tosdevflag }, TOS_MAJOR, 1 }
};

MOD_DRV_WRAPPER(tosguest, tosguest_load, tosguest_unload, 0, "Atari TOS guest");
