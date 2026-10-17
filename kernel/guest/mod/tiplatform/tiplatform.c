/*
 * tiplatform.c -- /dev/tiplatform and the Voyage 200 profile.
 *
 * AMS runs natively in user mode from its own reset vector, with the
 * supervisor state virtual (guestcore).  The CPU is a 68000 to it:
 * exceptions and interrupts push 6-byte frames through the vectors in
 * its RAM at 0, and rte pops them.  The register pages are memory;
 * the routines that need a live answer (key scan, idle halt, Flash
 * program and erase) are patched by the host to jump to service stubs
 * at TI_HOST, whose illegal instruction lands here.  TI_SIG carries the
 * interrupts from the clock tick, held while the guest's IPL masks them.
 *
 *	tick --> pend --> TI_SIG --> 6-byte frame --> AMS handler --> rte
 *	TI_KEYS (display process) --> tic.keys --> key scan service
 *
 * K&R C.
 */

#include "ti.h"
#include "sys/mman.h"
#include "vm/as.h"

extern int nodev(), ttimeout(), untimeout(), as_setprot();
extern void dlm_cacheflush(), flushmmu();
__asm__(".weak cputype");
extern long ti_cputype __asm__("cputype");	/* 30, 40, 60; absent: 68030 */
static long *__volatile__ cpup = &ti_cputype;
extern int __amix_sendsig(), __amix_fsig();
extern struct modwrapper tiplatform_wrapper;

static struct tictr {
	int		state;		/* 0 free, 2 claimed, 1 running */
	struct proc	*proc;
	struct guest_proc *gp;
	int		tid, sleeping, flags, opened, nnote, nexc;
	unsigned long	scanout, modtab;
	unsigned char	keys[10], on;
	unsigned int	pend;		/* bit n: autovector n waits */
	int		halt;		/* $600005 written: wake mask */
	unsigned long	walked;		/* tick of the last uncache() */
	struct ti_info	st;
} tic;
static struct guest_profile profile;
static int device_flag;
static void uncache();

static int
splhi_()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}

static void
splx_(s)
	int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

static int
vipl(gp)
	struct guest_proc *gp;
{
	return (gp->gp_vsr & SR_IPL) >> 8;
}

/* the highest autovector waiting, 0 none */
static int
level()
{
	int l;

	for (l = 7; l > 0; l--)
		if (tic.pend & 1 << l)
			return l;
	return 0;
}

static void
note(what, a, pc)
	char *what;
	long a, pc;
{
	tic.st.faults++;
	if (tic.nnote++ < 10)
		printf("NOTICE: Voyage %s %x pc %x\n", what, (int)a, (int)pc);
}

/* a request above the guest's IPL wakes a halted guest or sends the carrier */
static void
kick()
{
	int l;

	if (tic.state != 1)
		return;
	l = level();
	tic.gp->gp_vpend = l << 8;
	if (tic.sleeping)
		wakeup((caddr_t)&tic.sleeping);
	else if (l > vipl(tic.gp))
		psignal(tic.proc, TI_SIG);
}

/*
 * Autovector 1 every tick (the keyboard scan), 5 at 20 Hz (AMS's
 * timers and cursor), 3 at 1 Hz (the clock).
 */
static void
tick(arg)
	caddr_t arg;
{
	int s = splhi_();

	tic.tid = 0;
	if (tic.state != 1) {
		splx_(s);
		return;
	}
	tic.st.ticks++;
	tic.pend |= 1 << 1;
	if (tic.st.ticks % (HZ / 20) == 0)
		tic.pend |= 1 << 5;
	if (tic.st.ticks % HZ == 0)
		tic.pend |= 1 << 3;
	kick();
	splx_(s);
	if ((tic.tid = ttimeout(tick, (caddr_t)0, 1L)) == -1)
		tic.tid = 0;
}

/* a 68000 exception frame (SR, PC) through the vector at vec * 4 */
static int
reflect(gp, r, vec, ipl)
	struct guest_proc *gp;
	char *r;
	int vec, ipl;
{
	char f[6];
	long h, sp;
	int sr = guest_getsr(gp, r), nsr;

	nsr = (sr & ~0xc000) | SR_S;
	if (ipl >= 0)
		nsr = (nsr & ~SR_IPL) | ipl << 8;
	sp = (sr & SR_S) ? GR_USP(r) : gp->gp_vusp;
	sp -= 6;
	P16(f, sr);
	P32(f + 2, GR_PC(r));
	if (copyin((caddr_t)(vec * 4), (caddr_t)&h, 4) || copyout(f, (caddr_t)sp, 6))
		return -1;
	guest_setsr(gp, r, nsr);
	GR_USP(r) = sp;
	GR_PC(r) = h;
	return 0;
}

/* every interrupt the IPL lets through, each frame on top of the last */
static void
ti_intr(gp, r)
	struct guest_proc *gp;
	char *r;
{
	int s, l;

	if (tic.gp != gp || tic.state != 1)
		return;
	/* a fault frame resumes part way through its instruction */
	switch (GR_FV(r) >> 12) {
	case 7: case 9: case 0xa: case 0xb:
		if (!(u.u_sigflag & USTKCLEAR))
			return;
	}
	s = splhi_();
	while ((l = level()) > vipl(gp)) {
		tic.pend &= ~(1 << l);
		splx_(s);
		if ((tic.flags & TIF_CENSUS) && tic.st.ints < 8)
			printf("tiplatform: census interrupt %d pc %x sr %x\n", l, (int)GR_PC(r),
			    guest_getsr(gp, r));
		if (reflect(gp, r, l + 24, l) < 0) {
			note("interrupt without a stack", (long)l, GR_PC(r));
			psignal(curproc, SIGSEGV);
			return;
		}
		tic.st.ints++;
		if (GR_FV(r) >> 12)
			u.u_sigflag |= USTKCLEAR;
		s = splhi_();
	}
	gp->gp_vpend = l << 8;
	sigdelset(&gp->gp_proc->p_sig, TI_SIG);
	splx_(s);
}

/* the carrier waits while the guest's IPL masks every request */
static int
ti_fsig(p, gp)
	struct proc *p;
	struct guest_proc *gp;
{
	k_sigset_t h;
	int s, n;

	s = splhi_();
	h = p->p_hold;
	sigdelset(&p->p_hold, TI_SIG);
	if (tic.gp != gp || level() <= vipl(gp))
		sigaddset(&p->p_hold, TI_SIG);
	n = __amix_fsig(p);
	p->p_hold = h;
	splx_(s);
	return n;
}

static int
ti_sendsig(gp, sig, sip, hdlr)
	struct guest_proc *gp;
	int sig;
	char *sip;
	int (*hdlr)();
{
	if (sig != TI_SIG)
		return __amix_sendsig(sig, sip, hdlr);
	ti_intr(gp, (char *)u.u_ar0);
	return 1;
}

/* the guest sleeps until a request it waits for; then the trap tail */
static void
ti_after(gp, r)
	struct guest_proc *gp;
	char *r;
{
	int s, mask = tic.halt;

	if (tic.st.ticks - tic.walked >= HZ) {
		tic.walked = tic.st.ticks;
		uncache(curproc->p_as, TI_FLASH);
	}
	if (mask) {
		tic.halt = 0;
		s = splhi_();
		while (tic.state == 1 && !(tic.pend & mask)) {
			tic.sleeping = 1;
			if (sleep((caddr_t)&tic.sleeping, (PZERO + 1) | PCATCH))
				break;
		}
		tic.sleeping = 0;
		gp->gp_flags |= GPF_IDROP;
		splx_(s);
	}
	guest_trapret();
}

/*
 * AMS copies code into RAM and runs it (its Flash routines run at $5B02,
 * built afresh for each command); a 68000 has no caches, so nothing
 * flushes.  On a 68040/060 the copy would sit in the data cache while
 * the instruction cache fetches memory or an older routine.  So RAM is
 * cache-inhibited, serialized: every fetch sees the last store.  Flash
 * stays cached; it changes only in fput(), which pushes the caches.
 * A page refaulted after paging comes back cacheable, so the calculator's
 * pages are walked again each second.
 */
static void
uncache(as, hi)
	struct as *as;
	unsigned long hi;
{
	long *cpu = cpup;
	unsigned long *root, a, b, *leaf, va;
	int n = 0;

	if (!cpu || *cpu < 40 || !(root = *(unsigned long **)((char *)as + 20)))
		return;
	for (va = 0; va < hi; va += 0x1000) {
		if (!((a = root[va >> 25]) & 2) ||
		    !((b = ((unsigned long *)(a & 0xfffffe00UL))[(va >> 18) & 0x7f]) & 2))
			continue;
		leaf = (unsigned long *)(b & 0xffffff00UL) + ((va >> 12) & 0x3f);
		if ((*leaf & 1) && (*leaf & 0x60) != 0x40) {
			*leaf = (*leaf & ~0x60UL) | 0x40;
			flushmmu((addr_t)va, 1);
			n++;
		}
	}
	if (n)
		dlm_cacheflush();	/* lines the pages had go to memory */
}

/* ---- services ---- */

static int
gword(a)
	unsigned long a;
{
	unsigned char b[2];

	return copyin((caddr_t)a, (caddr_t)b, 2) ? 0 : G16(b);
}

static int
pword(a, v)
	unsigned long a;
	int v;
{
	unsigned char b[2];

	P16(b, v);
	return copyout((caddr_t)b, (caddr_t)a, 2);
}

/*
 * AMS's key scan: the one key down as row * 8 + column + 1 (ON $48,
 * arrows $50 + their bits), row 0's modifiers in the high byte; 0 and
 * d0.w clear for none or two keys.  Only modifiers: AMS's own table.
 */
static void
scan(r)
	char *r;
{
	int row, b, bits, code = 0, found = 0;

	if (tic.on)
		code = 0x48, found = 1;
	else {
		for (bits = 0, row = 0; row < 10; row++)
			bits |= tic.keys[row];
		if (!bits)
			goto none;
		for (row = 9; row > 0; row--) {
			if (!(bits = tic.keys[row]))
				continue;
			if (found)
				goto none;
			for (b = 7; !(bits & 1 << b); b--)
				;
			if (bits & ~(1 << b))
				goto none;
			code = row * 8 + b + 1;
			found = 1;
		}
	}
	if ((bits = tic.keys[0]) != 0) {
		if (bits >> 4) {
			if (found)
				goto none;
			code = 0x50 + (bits >> 4);
			found = 1;
		}
		code = found ? (bits & 15) << 8 | (code & 0xff) :
		    gword(tic.modtab + 2 * (bits & 15));
	}
	pword(tic.scanout, code);
	GR_D(r, 0) |= 0xffff;
	return;
none:
	pword(tic.scanout, 0);
	GR_D(r, 0) &= ~0xffffL;
}

/* a store: AMS's words land as written; a freed block is ones, AMS's empty */
static void
prog(r)
	char *r;
{
	unsigned long src = GR_A(r, 2), dst = GR_A(r, 3), n = GR_D(r, 4) & 0xffff;

	for (; n > 0; n--, src += 2, dst += 2)
		if (dst >= TI_FLASH && dst < TI_FLASH + TI_FLASH_SIZE)
			pword(TI_ALIAS + (dst - TI_FLASH), gword(src));
	GR_A(r, 2) = src;
	GR_A(r, 3) = dst;
	GR_D(r, 4) &= ~0xffffL;
	tic.st.flash++;
	dlm_cacheflush();
}

static void
sprog(r)
	char *r;
{
	unsigned long a[3];

	if (copyin((caddr_t)GR_USP(r) + 4, (caddr_t)a, sizeof a))
		return;
	if (a[1] < TI_FLASH || a[1] >= TI_FLASH + TI_FLASH_SIZE || a[2] > TI_FLASH_SIZE ||
	    a[1] + a[2] > TI_FLASH + TI_FLASH_SIZE)
		return;
	for (; (long)a[2] > 0; a[2] -= 2, a[0] += 2, a[1] += 2)
		pword(TI_ALIAS + (a[1] - TI_FLASH), gword(a[0]));
	tic.st.flash++;
	dlm_cacheflush();
}

static void
eblock(a)
	unsigned long a;
{
	static unsigned char ones[256];
	unsigned long i;

	if (!ones[0])
		for (i = 0; i < sizeof ones; i++)
			ones[i] = 0xff;
	a = TI_ALIAS + ((a - TI_FLASH) & ~0xffffUL);
	for (i = 0; i < 0x10000; i += sizeof ones)
		(void)copyout((caddr_t)ones, (caddr_t)(a + i), sizeof ones);
	tic.st.flash++;
}

static int
inflash(a)
	unsigned long a;
{
	return a >= TI_FLASH && a < TI_FLASH + TI_FLASH_SIZE;
}

static void
erase(r)
	char *r;
{
	unsigned long a;

	if (copyin((caddr_t)GR_USP(r) + 4, (caddr_t)&a, 4) || !inflash(a))
		return;
	eblock(a);
	GR_D(r, 0) = (GR_D(r, 0) & ~0xffffL) | 0x8080;
	dlm_cacheflush();
}

static void
move(r)
	char *r;
{
	static unsigned char b[256];
	unsigned long src, dst = GR_A(r, 1) & ~0xffffUL, i;

	if (copyin((caddr_t)GR_USP(r) + 4, (caddr_t)&src, 4) || !inflash(src) || !inflash(dst))
		return;
	src &= ~0xffffUL;
	eblock(dst);
	for (i = 0; i < 0x10000; i += sizeof b)
		if (copyin((caddr_t)(src + i), (caddr_t)b, sizeof b) == 0)
			(void)copyout((caddr_t)b, (caddr_t)(TI_ALIAS + (dst - TI_FLASH) + i), sizeof b);
	eblock(src);
	dlm_cacheflush();
}

/* ---- dispositions ---- */

/* an exception through the guest's own vector */
static int
exc(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	if ((tic.flags & TIF_CENSUS) && tic.nexc++ < 40)
		printf("tiplatform: census vector %d pc %x\n", v, (int)GR_PC(r));
	if (v == 3 || v == 4)
		note(v == 3 ? "address error" : "illegal instruction", (long)v, GR_PC(r));
	if (reflect(gp, r, v, -1) < 0) {
		note("exception without a stack", (long)v, GR_PC(r));
		psignal(curproc, SIGSEGV);
	}
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

static int
ti_badaccess(gp, r, a, what)
	struct guest_proc *gp;
	char *r;
	unsigned long a;
	char *what;
{
	note(what, (long)a, GR_PC(r));
	psignal(curproc, SIGBUS);
	guest_trapret();
	return 0;
}

/* a Flash store, or an access past Flash nothing is mapped at: AMS reached a routine nobody patched */
static int
hwfault(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned long fa;
	int ssw;

	if (guest_bfault(r, &fa, &ssw) < 0 || fa < TI_FLASH || TI_MAPPED(fa) ||
	    (fa < TI_FLASH + TI_FLASH_SIZE && (ssw & 0x100)))
		return 1;
	tic.st.io++;
	return ti_badaccess(gp, r, fa, "hardware access");
}

/* a stub at TI_HOST: the service, then its rts */
static int
service(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned long pc = GR_PC(r) - TI_HOST, buf;

	if (pc >= TS_N * 8 || pc % 8)
		return exc(gp, r, v);
	switch ((int)pc / 8) {
	case TS_SCAN:
		scan(r);
		break;
	case TS_MODS:
		GR_D(r, 0) = gword(tic.modtab + 2 * (tic.keys[0] & 15));
		break;
	case TS_ROWS:
		if (copyin((caddr_t)GR_USP(r) + 4, (caddr_t)&buf, 4) == 0)
			(void)copyout((caddr_t)tic.keys, (caddr_t)buf, 10);
		GR_D(r, 0) |= 0xffff;
		break;
	case TS_HALT:		/* off until one of autovectors 1-5, or ON */
		tic.halt = (GR_D(r, 0) & 0x1f) << 1 | 1 << 6;
		break;
	case TS_PROG:
		prog(r);
		break;
	case TS_ERASE:
		erase(r);
		break;
	case TS_WRITE:
		sprog(r);
		break;
	case TS_MOVE:
		move(r);
		break;
	case TS_DARK:
	case TS_LIT:
		tic.st.dark = (int)pc / 8 == TS_DARK;
		break;
	case TS_SIZE:
		GR_D(r, 0) = TI_FLASH_SIZE;
		GR_D(r, 1) = GR_A(r, 0) = TI_FLASH + TI_FLASH_SIZE;
		break;
	default:
		return exc(gp, r, v);
	}
	GR_PC(r) += 2;
	ti_after(gp, r);
	return 0;
}

/*
 * guestcore's emulation; GPF_PRIV only meanwhile, since with it the gate's
 * fast path would pop rte frames of a 68010 and later
 */
static int
emulate(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int e;

	gp->gp_flags |= GPF_PRIV;
	e = guest_priv(gp, r, v);
	gp->gp_flags &= ~GPF_PRIV;
	return e ? exc(gp, r, 4) : 0;
}

/* vector 8: what a 68000 in the guest's mode would do */
static int
priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned char b[6];
	long sp;
	int op;

	if (copyin((caddr_t)GR_PC(r), (caddr_t)b, 4))
		return exc(gp, r, 8);
	op = G16(b);
	if ((tic.flags & TIF_CENSUS) && tic.nexc++ < 60)
		printf("tiplatform: census priv %x pc %x sp %x vsr %x\n", op, (int)GR_PC(r),
		    (int)GR_USP(r), guest_getsr(gp, r));
	if ((op & 0xffc0) == 0x40c0)		/* move SR,<ea>: any mode on a 68000 */
		return emulate(gp, r, v);
	if (!(gp->gp_vsr & SR_S))
		return exc(gp, r, 8);
	switch (op) {
	case 0x4e73:				/* rte */
		sp = GR_USP(r);
		if (copyin((caddr_t)sp, (caddr_t)b, 6))
			return exc(gp, r, 3);
		GR_USP(r) = sp + 6;
		GR_PC(r) = G32(b + 2);
		guest_setsr(gp, r, (int)G16(b));
		break;
	case 0x4e72:				/* stop */
		guest_setsr(gp, r, (int)G16(b + 2));
		GR_PC(r) += 4;
		tic.halt = 0xfe;
		break;
	case 0x4e70:				/* reset */
		GR_PC(r) += 2;
		break;
	default:
		return emulate(gp, r, v);
	}
	ti_after(gp, r);
	return 0;
}

static void
ti_exit(gp)
	struct guest_proc *gp;
{
	if (tic.gp != gp)
		return;
	if (tic.tid > 0)
		untimeout(tic.tid);
	tic.tid = 0;
	tic.state = 0;
	tic.proc = 0;
	tic.gp = 0;
	if (tic.sleeping)
		wakeup((caddr_t)&tic.sleeping);
}

static int
ti_fork(parent, child)
	struct guest_proc *parent, *child;
{
	return EINVAL;
}

/* ---- the device ---- */

static int
enter(arg)
	caddr_t arg;
{
	struct ti_enter te;
	struct guest_proc *gp;
	int e, s;

	if (copyin(arg, (caddr_t)&te, sizeof te))
		return EFAULT;
	s = splhi_();
	if (tic.state) {
		splx_(s);
		return EBUSY;
	}
	tic.state = 2;
	splx_(s);
	if ((e = guest_attach(&profile)) != 0) {
		tic.state = 0;
		return e;
	}
	gp = GUESTP(curproc);
	gp->gp_vsr = 0x2700;
	gp->gp_vpend = 0;
	gp->gp_vusp = gp->gp_vvbr = gp->gp_vcacr = 0;
	bzero((caddr_t)&tic.st, sizeof tic.st);
	tic.st.lcd = TI_LCD;
	tic.pend = tic.halt = tic.nnote = tic.nexc = tic.sleeping = 0;
	tic.flags = te.flags;
	tic.scanout = te.scanout;
	tic.modtab = te.modtab;
	tic.proc = curproc;
	tic.gp = gp;
	tic.walked = 0;
	uncache(curproc->p_as, TI_FLASH);
	dlm_cacheflush();
	tic.state = 1;
	if ((tic.tid = ttimeout(tick, (caddr_t)0, 1L)) == -1)
		tic.tid = 0;
	return 0;
}

static int
keys(arg)
	caddr_t arg;
{
	struct ti_keys k;
	int i, s, down = 0;

	if (copyin(arg, (caddr_t)&k, sizeof k))
		return EFAULT;
	s = splhi_();
	for (i = 0; i < 10; i++) {
		down |= k.rows[i] & ~tic.keys[i];
		tic.keys[i] = k.rows[i];
	}
	if (down)
		tic.pend |= 1 << 2;
	if (k.on && !tic.on)
		tic.pend |= 1 << 6;
	tic.on = k.on;
	kick();
	splx_(s);
	return 0;
}

static int
open_device(dev, flags, kind, credentials)
	dev_t *dev;
	int flags, kind;
	struct cred *credentials;
{
	if (getminor(*dev))
		return ENXIO;
	tic.opened++;
	return 0;
}

static int
close_device(dev, flags, kind, credentials)
	dev_t dev;
	int flags, kind;
	struct cred *credentials;
{
	tic.opened = 0;
	return 0;
}

static int
ioctl_device(dev, command, argument, mode, credentials, result)
	dev_t dev;
	int command, mode;
	caddr_t argument;
	struct cred *credentials;
	int *result;
{
	switch (command) {
	case TI_ENTER:
		return GUESTP(curproc) ? EBUSY : enter(argument);
	case TI_KEYS:
		return tic.state == 1 ? keys(argument) : ENXIO;
	case TI_INFO:
		if (tic.state != 1)
			return ENXIO;
		if (curproc != tic.proc)	/* the display process reads the LCD */
			uncache(curproc->p_as, (tic.st.lcd + 3840 + 0xfff) & ~0xfffUL);
		return copyout((caddr_t)&tic.st, argument, sizeof tic.st) ? EFAULT : 0;
	}
	return EINVAL;
}

static int
load()
{
	struct guest_disp *d;
	int v;

	bzero((caddr_t)&profile, sizeof profile);
	profile.gpf_name = "voyage200";
	profile.gpf_wrapper = &tiplatform_wrapper;
	profile.gpf_exit = ti_exit;
	profile.gpf_fork = ti_fork;
	profile.gpf_fsig = ti_fsig;
	profile.gpf_sendsig = ti_sendsig;
	profile.gpf_intr = ti_intr;
	for (v = 2; v < 48; v++) {
		if (v > 11 && v < 32)
			continue;
		d = &profile.gpf_disp[v];
		d->gd_kind = GD_EMULATE;
		d->gd_flags = GDF_USER;
		d->gd_fn = v == 2 ? hwfault : v == 4 ? service : v == 8 ? priv : exc;
	}
	return guest_profile_add(&profile);
}

static int
unload()
{
	if (tic.state || tic.opened)
		return EBUSY;
	guest_profile_del(&profile);
	return 0;
}

struct mod_drv_data tiplatform_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	{ open_device, close_device, nodev, nodev, ioctl_device, nodev, nodev,
		nodev, nodev, nodev, 0, 0, &device_flag }, TI_MAJOR, 1 }
};
MOD_DRV_WRAPPER(tiplatform, load, unload, 0, "Voyage 200 native services");
