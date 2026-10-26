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
extern void dlm_cacheflush();
extern long lbolt;
extern struct modwrapper tosguest_wrapper;
extern struct proc *prfind();

struct tosctr tosc;
int	tos_trace = 0;		/* 1: console lines for bus errors and odd accesses */

static struct guest_profile tos_profile;
static int tos_nopen;
static struct guest_ramdev tos_rd;	/* the ST-RAM block */
static int tosdevflag[1] = { 0 };

/* the highest level asking, 0 none */
static int
tos_level()
{
	if (mfp_level())
		return 6;
	return tosc.t_vblpend ? 4 : 0;
}

/* the highest level the guest can take now */
static int
tos_dlevel()
{
	if (mfp_ready())
		return 6;
	return tosc.t_vblpend ? 4 : 0;
}

#define	PVTCSVC	(caddr_t)(TOSPV + (int)&((struct tospv *)0)->pv_tcsvc)

/*
 * Timer C stays in service, as on the MFP, until its handler's end
 * clears pv_tcsvc: a handler that lowers the IPL is not entered again.
 * A second at most, for a handler that never gets there.
 */
static void
tos_tcpoll(gp)
	struct guest_proc *gp;
{
	struct tosctr *t = &tosc;

	if (!t->t_tcsvc)
		return;
	if (fuword(PVTCSVC) <= 0 || lbolt - t->t_tcsince > HZ)
		t->t_tcsvc = 0;
	else if (t->t_ipend[CH_TIMERC] && GUEST_VIPL(gp) < 6)
		t->t_st.ts_tcheld++;
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

	if (t->t_state != 1 || t->t_paused)
		return;
	/* a Timer C in service waits for the IPL drop at its handler's end */
	t->t_gp->gp_vpend = tos_level() << 8;
	if (tos_dlevel() <= GUEST_VIPL(t->t_gp))
		return;
	if (t->t_sleeping)
		wakeup((caddr_t)&t->t_sleeping);
	else {
		psignal(t->t_proc, TOS_SIG);
		if (rr)
			runrun = 1;
	}
}

static int tos_pauser();

/* stop: the guest sleeps until an interrupt above its new IPL is pending */
static void
tos_stop_insn(gp, r)
	struct guest_proc *gp;
	char *r;
{
	struct tosctr *t = &tosc;
	int s, e, id;
	long t0 = lbolt;

	if (gp->gp_flags & TGF_SOLO) {
		guest_trapret();
		return;
	}
	t->t_st.ts_stop++;
	for (;;) {
		tos_tcpoll(gp);		/* a Timer C still in service does not end it */
		s = guest_splhi();
		if (t->t_paused && !tos_pauser())
			t->t_paused = 0;
		/* paused, nothing is taken: sleep on, checking each second that the pauser runs */
		if (t->t_state != 1 || (!t->t_paused && tos_dlevel() > GUEST_VIPL(gp)))
			break;
		t->t_sleeping = 1;
		id = t->t_paused ? ttimeout(wakeup, (caddr_t)&t->t_sleeping, (long)HZ) : -1;
		e = sleep((caddr_t)&t->t_sleeping, (PZERO + 1) | PCATCH);
		if (id != -1)
			untimeout(id);
		t->t_sleeping = 0;
		if (e)
			break;
		guest_splx(s);
	}
	t->t_sleeping = 0;
	gp->gp_flags |= GPF_IDROP;	/* the CPU takes the interrupt that ends stop */
	t->t_st.ts_slept += lbolt - t0;
	guest_splx(s);
	guest_trapret();
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
	if (t->t_gp != gp || GUEST_INHOST(gp, r))
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
	tos_tcpoll(gp);
	s = guest_splhi();
	while (!t->t_paused && (l = tos_dlevel()) > GUEST_VIPL(gp)) {
		/* the tick posts the carrier again; meanwhile the gate's fast paths stay open */
		if (tos_hold(gp)) {
			sigdelset(&gp->gp_proc->p_sig, TOS_SIG);
			break;
		}
		t->t_held = 0;
		guest_splx(s);
		tos_unsink();
		s = guest_splhi();
		if (l == 6) {
			vec = mfp_ack();
			guest_splx(s);
		} else {
			if (t->t_vblowed)
				t->t_vblowed--;
			else
				t->t_vblpend = 0;
			t->t_st.ts_vbl++;
			vec = 28;
			guest_splx(s);
		}
		if (guest_reflect(gp, r, GR_PC(r), vec << 2, (char *)0, 2, l) < 0) {
			s = guest_splhi();
			if (l == 6)
				mfp_unack(vec);		/* still pending for a debugger */
			else
				t->t_vblpend = 1;
			guest_splx(s);
			printf("tos: interrupt with no guest stack, pc %x\n", (int)GR_PC(r));
			psignal(curproc, SIGSEGV);
			return;
		}
		if (vec == 0x40 + CH_TIMERC && (t->t_flags & TEF_TCEOI) && suword(PVTCSVC, 1) == 0) {
			t->t_tcsvc = 1;
			t->t_tcsince = lbolt;
		}
		if (GR_FV(r) >> 12)
			u.u_sigflag |= USTKCLEAR;
		s = guest_splhi();
	}
	gp->gp_vpend = tos_level() << 8;
	if (!t->t_paused && tos_dlevel() <= GUEST_VIPL(gp)) {
		t->t_held = 0;
		sigdelset(&gp->gp_proc->p_sig, TOS_SIG);
	}
	guest_splx(s);
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
	s = guest_splhi();
	h = p->p_hold;
	sigdelset(&p->p_hold, TOS_SIG);
	if (tosc.t_gp != gp || (!tosc.t_paused && (tos_dlevel() <= GUEST_VIPL(gp) || tos_hold(gp))))
		sigaddset(&p->p_hold, TOS_SIG);
	n = __amix_fsig(p);
	p->p_hold = h;
	guest_splx(s);
	return n;
}

/* the process that paused the guest still runs */
static int
tos_pauser()
{
	struct proc *p = prfind(tosc.t_ppid);

	return p && p == tosc.t_pproc && p->p_stat != SZOMB;
}

/* a paused guest sleeps until resumed, its pauser exits or a signal comes */
static void
tos_pausewait(gp)
	struct guest_proc *gp;
{
	int s, id, e;

	if (!tosc.t_paused || tosc.t_gp != gp)
		return;
	s = guest_splhi();
	while (tosc.t_paused && tosc.t_gp == gp) {
		if (!tos_pauser()) {
			tosc.t_paused = 0;
			break;
		}
		id = ttimeout(wakeup, (caddr_t)&tosc.t_paused, (long)HZ);
		e = sleep((caddr_t)&tosc.t_paused, (PZERO + 1) | PCATCH);
		if (id != -1)
			untimeout(id);
		if (e)
			break;
	}
	guest_splx(s);
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

	if (sig != TOS_SIG || (gp->gp_flags & TGF_SOLO))
		return __amix_sendsig(sig, sip, hdlr);
	if (tosc.t_gp != gp)
		return 1;
	tos_pausewait(gp);
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
	if (gp->gp_flags & TGF_SOLO)
		return guest_lone_trap(gp, r, v);
	tos_pausewait(gp);
	if (!guest_fvsize(GR_FV(r)))
		return 1;
	TST(gp)->ts_lastpc = GR_PC(r);
	if (v < 64)
		TST(gp)->ts_refl[v]++;
	return guest_reflcpu(gp, r, GR_PC(r), (int)GR_FV(r), "tos");
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
	return guest_reflcpu(gp, r, pc, v << 2, "tos");
}

/* vectors 8 and 11: user mode and what guestcore does not emulate reflect */
static int
tos_priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int e;

	if (gp->gp_flags & TGF_SOLO)
		return guest_lone_trap(gp, r, v);
	tos_pausewait(gp);
	TST(gp)->ts_lastpc = GR_PC(r);
	if ((e = guest_privtrap(gp, r, v)) == 1)
		return 1;
	if (e == v)
		return tos_refl(gp, r, v);
	if (e)
		return tos_refl0(gp, r, e, GR_PC(r));
	if (gp->gp_pkind == GPK_MMU)
		tosc.t_st.ts_mmu++;
	else
		TST(gp)->ts_priv++;
	if (gp->gp_pkind == GPK_CACHE)
		TST(gp)->ts_cache++;	/* cinv, cpush, CACR: pushed for real */
	return 0;
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
	    (unsigned long)GR_USP(r) - gp->gp_hstklo >= gp->gp_hstkhi - gp->gp_hstklo)
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
	int s, n;

	t->t_tid = 0;
	if (t->t_state != 1)
		return;
	/* the callout can run a tick late: the timers count clock ticks, not runs */
	n = lbolt - t->t_tickbolt;
	t->t_tickbolt = lbolt;
	s = guest_splhi();
	if (t->t_vblpend && curproc == t->t_proc && GUEST_VIPL(t->t_gp) >= 4)
		t->t_st.ts_vblheld++;
	else if (t->t_vblpend && t->t_held && t->t_vblowed < 2)
		t->t_vblowed++;		/* goes out after the held one */
	t->t_vblpend = 1;
	tos_timers(n < HZ ? n : HZ);
	guest_splx(s);
	tos_kick(1);
	if ((t->t_tid = ttimeout(tos_tick, (caddr_t)0, 1L)) == -1)
		t->t_tid = 0;
}

static void
tos_stop()
{
	struct tosctr *t = &tosc;
	int s = guest_splhi();

	if (t->t_tid) {
		untimeout(t->t_tid);
		t->t_tid = 0;
	}
	t->t_state = 0;
	t->t_proc = 0;
	t->t_gp = 0;
	guest_splx(s);
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
	guest_ramdev_free(&tos_rd);
	return 0;
}

/* the passthrough ST-RAM block */
int
tosmmap(dev, off, prot)
	dev_t dev;
	off_t off;
	int prot;
{
	return guest_ramdev_mmap(&tos_rd, off);
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
	return guest_ramdev_segmap(&tos_rd, dev, off, as, addrp, len, prot, maxprot, flags);
}

static int
stram(size, flags)
unsigned long size;
int flags;
{
	if (GUESTP(curproc))
		return EPERM;
	if (tosc.t_state != 0)
		return EBUSY;
	return guest_ramdev_new(&tos_rd, size, flags, 0xE00000UL);
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

	if (!guest_ramdev_mine(&tos_rd))
		return -1;
	seg = as_segat(curproc->p_as, (caddr_t)(va >= 0xfffff000 ? 0xfff000 : va));
	if (!seg || (va >= 0xfffff000 && seg->s_base != (addr_t)0xfff000))
		return -1;
	for (n = 0; n < IOPG; n += step) {
		if ((pf = guest_ram_frame(tos_rd.rd_ram, off + n)) < 0 || !(pp = page_numtookpp((u_int)pf)))
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
		if ((f = guest_ram_frame(tos_rd.rd_ram, off + n)) < 0 || (k = guest_ram_frame(tos_rd.rd_ram, to + n)) < 0 ||
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
	s = guest_splhi();
	if (t->t_state != 0) {
		guest_splx(s);
		return EBUSY;
	}
	if (!guest_ramdev_mine(&tos_rd)) {
		guest_splx(s);
		return EACCES;		/* only the RAM's owner maps the I/O pages */
	}
	t->t_state = 2;			/* claimed: the tick and hooks wait for 1 */
	guest_splx(s);
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
	gp->gp_hstklo = te.te_stklo;
	gp->gp_hstkhi = te.te_stkhi;
	bzero((caddr_t)&t->t_proc, sizeof *t - ((caddr_t)&t->t_proc - (caddr_t)t));
	t->t_proc = curproc;
	t->t_gp = gp;
	t->t_uid = cr->cr_uid;
	t->t_ramsize = te.te_ramsize;
	t->t_flags = te.te_flags;
	t->t_ien = 0xffff;
	t->t_st.ts_pid = curproc->p_pid;
	tos_devinit(t);
	(void)tos_toppage();
	dlm_cacheflush();		/* the launcher's ROM and cartridge stores reach memory */
	t->t_state = 1;
	t->t_tickbolt = lbolt;
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
		s = guest_splhi();
		tv.tv_vbl = t->t_st.ts_vbl;
		tv.tv_vblheld = t->t_st.ts_vblheld;
		guest_splx(s);
		return copyout((caddr_t)&tv, arg, sizeof tv) ? EFAULT : 0;
	case TOSIOC_PAUSE:
		s = guest_splhi();
		t->t_paused = arg != 0;
		t->t_ppid = curproc->p_pid;
		t->t_pproc = curproc;
		if (t->t_paused)
			psignal(t->t_proc, TOS_SIG);
		else
			wakeup((caddr_t)&t->t_paused);
		guest_splx(s);
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

/* vector 61 on a 68060, its accesses resolved as tos_fault's */
int
tos_isp(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned long fa;
	int ssw, e, n;

	for (n = 0; n < 4; n++) {
		if ((e = guest_isp(gp, r, &fa, &ssw)) == -1)
			return 1;
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
	tos_profile.gpf_stop = tos_stop_insn;
	for (v = 2; v < 48; v++) {
		if (v > 11 && v < 32)
			continue;
		d = &tos_profile.gpf_disp[v];
		d->gd_flags = GDF_USER;
		d->gd_kind = v == 2 || v == 8 || v == 11 ? GD_EMULATE : GD_REFLECT;
		d->gd_fn = v == 2 ? tos_fault : v == 8 || v == 11 ? tos_priv : tos_refl;
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
