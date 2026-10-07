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
#include "hsock.h"

extern int nodev(), ttimeout(), untimeout();
extern int runrun;
extern int fpu_present;
extern void dlm_cacheflush();
extern long lbolt;
extern struct modwrapper tosguest_wrapper;
extern struct proc *prfind();
extern int tos_maprom();
/* weak, absent on the Mac: read at run time, as the compiler takes a declared symbol's address as nonzero */
__asm__(".weak ds_sndexit");
extern void ds_sndexit();
static void (*volatile sndexit_p)() = ds_sndexit;
__asm__(".weak ds_sndcb");
extern void (*ds_sndcb)();
static void (*volatile *volatile sndcb_p)() = &ds_sndcb;
__asm__(".weak ds_gralloc");
extern int ds_gralloc();
static int (*volatile gralloc_p)() = ds_gralloc;
__asm__(".weak ds_grfree");
extern void ds_grfree();
static void (*volatile grfree_p)() = ds_grfree;
__asm__(".weak ds_grmmap");
extern int ds_grmmap();
static int (*volatile grmmap_p)() = ds_grmmap;
/* the weak ones above are absent on the Mac; the compiler would take their addresses as nonzero */
extern int spec_segmap();
extern struct seg *as_segat();
extern page_t *page_numtookpp();
#if defined(DS_ATARI) && !defined(ATA060)
#define ST_PGSIZE	0x800		/* the block's page frames, as ds_grmmap counts them */
#else
#define ST_PGSIZE	0x1000
#endif

struct tosctr tosc;
int	tos_trace = 0;		/* 1: console lines for bus errors and odd accesses */

static struct guest_profile tos_profile;
static int tos_nopen;
static struct proc *tos_stproc;	/* allocated the ST-RAM block; only it maps it */
static pid_t tos_stpid;
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

	if (t->t_gp != gp)
		return;
	/*
	 * A fault frame resumes its instruction part way through; restarting
	 * it would repeat its side effects.  The CPU takes interrupts between
	 * instructions, so these wait for the next tick.
	 */
	switch (GR_FV(r) >> 12) {
	case 9: case 0xa: case 0xb:
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
		if (l == 6)
			vec = mfp_ack();
		else {
			if (t->t_vblowed)
				t->t_vblowed--;
			else
				t->t_vblpend = 0;
			t->t_st.ts_vbl++;
			vec = 28;
		}
		splx_(s);
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

/* a lone guest's empty vector: the Unix signal instead */
static int
solonull(gp, v)
	struct guest_proc *gp;
	int v;
{
	return (gp->gp_flags & TGF_SOLO) && fuword((caddr_t)(gp->gp_vvbr + (v << 2))) == 0;
}

static int tos_refl0();

/*
 * A lone guest's empty vector: a trap or line A/F takes the illegal
 * instruction vector, and the Unix signal when that is empty too.
 */
static int
solotrap(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	if (v < 10 || v == 32 || (v > 11 && v < 32))
		return 1;
	if (fuword((caddr_t)(gp->gp_vvbr + 16)) != 0)
		return tos_refl0(gp, r, 4, v > 32 ? GR_PC(r) - 2 : GR_PC(r));
	psignal(curproc, SIGILL);
	guest_trapret();
	return 0;
}

/* the CPU's own frame, through the guest's vector */
static int
tos_refl(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int n;

	if (solonull(gp, v))
		return solotrap(gp, r, v);
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
	if (solonull(gp, v))
		return 1;
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

	TST(gp)->ts_lastpc = GR_PC(r);
	if (!(gp->gp_vsr & SR_S))
		return tos_refl(gp, r, v);
	if ((opword(r) & 0xffc0) == 0xf000)	/* a 68030's MMU instructions */
		return tos_fline(gp, r, v);
	if (!fpu_present && (opword(r) & 0xfe00) == 0xf200 && (opword(r) >> 7 & 3) == 2)
		return tos_refl0(gp, r, 11, GR_PC(r));	/* fsave, frestore: no FPU */
	TST(gp)->ts_priv++;
	if (opword(r) == 0x4e70) {		/* reset */
		GR_PC(r) += 2;
		guest_trapret();
		return 0;
	}
	if (opword(r) == 0x4e72 && copyin((caddr_t)GR_PC(r) + 2, sr, 2) == 0) {
		guest_setsr(gp, r, (int)G16(sr));
		GR_PC(r) += 4;
		if (!(gp->gp_flags & TGF_SOLO))
			tos_stop_insn(gp);
		guest_trapret();
		return 0;
	}
	if (opword(r) >> 8 == 0xf4 || (opword(r) == 0x4e7b &&
	    copyin((caddr_t)GR_PC(r) + 2, sr, 2) == 0 && (G16(sr) & 0xfff) == 2))
		TST(gp)->ts_cache++;	/* cinv, cpush, CACR: pushed for real */
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

	TST(gp)->ts_lastpc = GR_PC(r);
	if ((op & 0xffc0) == 0xf000 && !(gp->gp_flags & TGF_SOLO)) {
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

/* trap #0: a host system call from the machine layer; anyone else's goes through the vector */
static int
tos_sys(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	if (!(gp->gp_flags & TGF_SOLO) && ((unsigned long)GR_PC(r) < 0xfa0000 ||
	    (unsigned long)GR_PC(r) >= 0xfc0000))
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
	if (tosc.t_gp == gp) {
		if (sndexit_p)
			sndexit_p(gp->gp_proc);
		tos_stop();
	}
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
	if (grfree_p)
		grfree_p();		/* after the last mapping too */
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
	return grmmap_p ? grmmap_p(off) : -1;
}

/*
 * The block is managed memory: its faults load it with its page
 * structures, so that the mapping lists the exit's teardown walks hold
 * every translation.  segdev would load it as device memory.
 */
static struct seg_ops st_segops, *st_devops;

static faultcode_t
st_fault(seg, addr, len, type, rw)
	struct seg *seg;
	addr_t addr;
	u_int len;
	enum fault_type type;
	enum seg_rw rw;
{
	struct segdev_data *sd = (struct segdev_data *)seg->s_data;
	unsigned long va;
	page_t *pp;
	u_int pv[1];
	int pf;

	if (type != F_INVAL && type != F_SOFTLOCK)
		return (*st_devops->fault)(seg, addr, len, type, rw);
	for (va = (unsigned long)addr & ~(ST_PGSIZE - 1); va < (unsigned long)addr + len;
	    va += ST_PGSIZE) {
		pv[0] = 0;
		(void)(*st_devops->getprot)(seg, (addr_t)va, 0, pv);
		if ((pv[0] & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0 ||
		    (rw == S_WRITE && !(pv[0] & PROT_WRITE)))
			return FC_PROT;
		pf = tosmmap(sd->vp->v_rdev, (off_t)(sd->offset + (va - (unsigned long)seg->s_base)), (int)pv[0]);
		if (pf == -1 || (pp = page_numtookpp((u_int)pf)) == 0)
			return FC_MAKE_ERR(EFAULT);
		hat_memload(seg, (addr_t)va, pp, pv[0], type == F_SOFTLOCK ? HAT_LOCK : HAT_NOFLAGS);
	}
	return 0;
}

static int
st_dup(seg, nseg)
	struct seg *seg, *nseg;
{
	int e;

	if ((e = (*st_devops->dup)(seg, nseg)) == 0)
		nseg->s_ops = &st_segops;
	return e;
}

/* a hole in the middle leaves a second segment, made with the device's operations */
static int
st_unmap(seg, addr, len)
	struct seg *seg;
	addr_t addr;
	u_int len;
{
	struct as *as = seg->s_as;
	struct seg *n;
	int mid = addr > seg->s_base && addr + len < seg->s_base + seg->s_size;
	int e;

	if ((e = (*st_devops->unmap)(seg, addr, len)) == 0 && mid &&
	    (n = as_segat(as, addr + len)) != 0 && n->s_ops == st_devops)
		n->s_ops = &st_segops;
	return e;
}

static void
st_wrap(seg)
	struct seg *seg;
{
	if (st_devops == 0) {
		st_devops = seg->s_ops;
		st_segops = *st_devops;
		st_segops.fault = st_fault;
		st_segops.dup = st_dup;
		st_segops.unmap = st_unmap;
	}
	seg->s_ops = &st_segops;
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
	struct seg *seg;
	int e;

	if (curproc != tos_stproc || curproc->p_pid != tos_stpid)
		return EACCES;
	if ((e = spec_segmap(dev, off, as, addrp, len, prot, maxprot, flags, cr)) == 0 &&
	    (seg = as_segat(as, *addrp)) != 0)
		st_wrap(seg);
	return e;
}

static int
stram(size)
	unsigned long size;
{
	int e;

	if (!gralloc_p)
		return ENXIO;
	if (GUESTP(curproc))
		return EPERM;
	if ((e = gralloc_p(size)) == 0) {
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
	gp->gp_flags |= GPF_PRIV | GPF_FTRAP;
	gp->gp_vsr = 0x2700;
	gp->gp_vpend = 0;
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

/* a guest of its own: no machine, no tick, any number of them */
static int
solo()
{
	struct guest_proc *gp;
	int e;

	if ((e = guest_attach(&tos_profile)) != 0)
		return e;
	gp = GUESTP(curproc);
	gp->gp_flags |= GPF_PRIV | GPF_FTRAP | TGF_SOLO;
	gp->gp_vsr = 0x2700;
	gp->gp_vpend = 0;
	gp->gp_vusp = gp->gp_vvbr = gp->gp_vcacr = 0;
	return 0;
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
	struct tosinput ti;
	struct tosvideo *tv;
	int e, s;

	if (cmd == TOSIOC_ENTER)
		return enter(arg, cr);
	if (cmd == TOSIOC_MAPROM)
		return GUESTP(curproc) && GUESTP(curproc)->gp_prof != &tos_profile ?
		    EPERM : tos_maprom(dev, rvp);
	if (cmd == TOSIOC_STRAM)
		return stram((unsigned long)arg);
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
		tv->tv_vblheld = t->t_st.ts_vblheld;
		splx_(s);
		e = copyout((caddr_t)tv, arg, sizeof *tv) ? EFAULT : 0;
		kmem_free((caddr_t)tv, sizeof *tv);
		return e;
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
	case TOSIOC_HALT:
		return curproc == t->t_proc ? guest_halt() : EPERM;
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
	if (sndcb_p)
		*sndcb_p = tos_sndend;
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
	return guest_profile_add(&tos_profile);
}

static int
tosguest_unload()
{
	if (tosc.t_state != 0 || tos_nopen)
		return EBUSY;
	guest_profile_del(&tos_profile);
	if (sndcb_p)
		*sndcb_p = 0;
	return 0;
}

struct mod_drv_data tosguest_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	  { tosopen, tosclose, tosrdwr, tosrdwr, tosioctl, tosmmap, tossegmap, nodev,
	    nodev, nodev, 0, 0, tosdevflag }, TOS_MAJOR, 1 }
};

MOD_DRV_WRAPPER(tosguest, tosguest_load, tosguest_unload, 0, "Atari TOS guest");
