/*
 * tiplatform.c -- /dev/tiplatform and the Voyage 200 profile.
 *
 * AMS runs natively in user mode from its own reset vector, with the
 * supervisor state virtual (guestcore).  The CPU is a 68000 to it:
 * exceptions and interrupts push 6-byte frames through the vectors in
 * its RAM at 0, and rte pops them.  The register pages are emulated
 * (tifault.c decodes the access): the LCD base, the keyboard matrix
 * from TI_KEYS, the timers from the clock tick, Flash commands applied
 * to the Flash file through its writable alias.  TI_SIG carries the
 * interrupts, held while the guest's IPL masks them.
 *
 *	tick --> pend --> TI_SIG --> 6-byte frame --> AMS handler --> rte
 *	TI_KEYS (display process) --> matrix --> $60001B reads
 *
 * K&R C.
 */

#include "ti.h"
#include "sys/mman.h"
#include "vm/as.h"

extern int nodev(), ttimeout(), untimeout(), as_setprot();
extern void dlm_cacheflush();
extern int __amix_sendsig(), __amix_fsig();
extern struct modwrapper tiplatform_wrapper;

/* Flash modes */
#define	FM_READ		0
#define	FM_STATUS	1
#define	FM_PROG		2
#define	FM_ERASE	3
#define	FM_ID		4

static struct tictr {
	int		state;		/* 0 free, 2 claimed, 1 running */
	struct proc	*proc;
	struct guest_proc *gp;
	int		tid, sleeping, flags, opened, nnote, nexc;
	unsigned char	io[2][32];	/* $600000, $700000, mirrored every 32 bytes */
	unsigned char	keys[10], on;
	unsigned int	pend;		/* bit n: autovector n waits */
	int		halt;		/* $600005 written: wake mask */
	unsigned long	cmdblk;		/* Flash block answering status, 0 none */
	int		fmode;
	unsigned char	seen[2][32];	/* census: bit 0 read, bit 1 write */
	struct ti_info	st;
} tic;
static struct guest_profile profile;
static int device_flag;

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
void
ti_after(gp, r)
	struct guest_proc *gp;
	char *r;
{
	int s, mask = tic.halt;

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

/* ---- registers ---- */

static void
census(bank, o, wr, v)
	int bank, o, wr;
	unsigned long v;
{
	if (!(tic.flags & TIF_CENSUS) || (tic.seen[bank][o] & (1 << wr)))
		return;
	tic.seen[bank][o] |= 1 << wr;
	printf("tiplatform: census %s %x pc %x value %x\n", wr ? "write" : "read",
	    (int)(TI_IO + (bank << 20) + o), (int)GR_PC(u.u_ar0), (int)v);
}

static int
iorb(a)
	unsigned long a;
{
	int bank = (a >> 20) & 1, o = a & 0x1f, v = tic.io[bank][o], i, mask;

	if (bank == 0 && o == 0x1b) {		/* columns of the rows selected low */
		mask = tic.io[0][0x18] << 8 | tic.io[0][0x19];
		v = 0xff;
		for (i = 0; i < 10; i++)
			if (!(mask & 1 << i))
				v &= ~tic.keys[i];
	} else if (bank == 0 && o == 0x1a)	/* bit 1 low: ON is down */
		v = tic.on ? v & ~2 : v | 2;
	else if (bank == 0 && o == 0)		/* the battery comparator: above every threshold */
		v |= 4;
	census(bank, o, 0, (unsigned long)v);
	return v;
}

static void
iowb(a, v)
	unsigned long a;
	int v;
{
	int bank = (a >> 20) & 1, o = a & 0x1f;

	census(bank, o, 1, (unsigned long)v);
	tic.io[bank][o] = v;
	if (bank == 0 && o == 0x05)	/* off until one of autovectors 1-5, or ON */
		tic.halt = (v & 0x1f) << 1 | 1 << 6;
	if (bank == 0 && o == 0x11)
		tic.st.lcd = (tic.io[0][0x10] << 8 | v) << 3;
	if (bank == 0 && o == 0x1d)
		tic.st.contrast = v & 0x1f;
}

/* ---- Flash: block erase and word program, status in bit 7 ---- */

static void
setprot(blk, prot)
	unsigned long blk;
	int prot;
{
	as_setprot(curproc->p_as, (caddr_t)blk, 0x10000, prot | PROT_USER);
}

/* 1: reads at a give the command state */
int
ti_cmdblock(a)
	unsigned long a;
{
	return tic.cmdblk && (a & ~0xffffUL) == tic.cmdblk;
}

static void
fmode(a, m)
	unsigned long a;
	int m;
{
	unsigned long blk = m == FM_READ ? 0 : a & ~0xffffUL;

	if (blk != tic.cmdblk) {
		if (tic.cmdblk)
			setprot(tic.cmdblk, PROT_READ | PROT_EXEC);
		if (blk)
			setprot(blk, 0);
		tic.cmdblk = blk;
	}
	tic.fmode = m;
}

static int
fput(a, v)
	unsigned long a;
	int v;
{
	static unsigned char ones[256];
	unsigned char b[2], o[2];
	unsigned long alias = TI_ALIAS + (a - TI_FLASH), i;
	int c = v & 0xff;

	tic.st.flash++;
	switch (tic.fmode) {
	case FM_PROG:
		if (copyin((caddr_t)alias, (caddr_t)o, 2))
			return -1;
		b[0] = o[0] & v >> 8;
		b[1] = o[1] & v;
		if (copyout((caddr_t)b, (caddr_t)alias, 2))
			return -1;
		fmode(a, FM_STATUS);
		dlm_cacheflush();
		return 0;
	case FM_ERASE:
		if (c == 0xd0) {
			if (!ones[0])
				for (i = 0; i < sizeof ones; i++)
					ones[i] = 0xff;
			alias &= ~0xffffUL;
			for (i = 0; i < 0x10000; i += sizeof ones)
				if (copyout((caddr_t)ones, (caddr_t)(alias + i), sizeof ones))
					return -1;
			dlm_cacheflush();
		}
		fmode(a, FM_STATUS);
		return 0;
	}
	switch (c) {
	case 0xff: fmode(a, FM_READ); break;
	case 0x50: case 0x70: fmode(a, FM_STATUS); break;
	case 0x10: case 0x40: fmode(a, FM_PROG); break;
	case 0x20: fmode(a, FM_ERASE); break;
	case 0x90: fmode(a, FM_ID); break;
	default: note("Flash command", (long)v, GR_PC(u.u_ar0));
	}
	return 0;
}

static unsigned long
fget(a)
	unsigned long a;
{
	if (tic.fmode == FM_ID)
		return a & 2 ? 0x00d4 : 0x00b0;
	return 0x8080;		/* ready, no errors */
}

/* the faulted access: 0 done, -1 a bus error */
int
ti_read(a, sz, vp)
	unsigned long a, *vp;
	int sz;
{
	unsigned long v = 0;
	int i;

	if (a >= TI_FLASH && a < TI_FLASH + TI_FLASH_SIZE) {
		for (i = 0; i < sz; i += 2)
			v = v << 16 | fget(a + i);
		*vp = sz == 1 ? (a & 1 ? v & 0xff : v >> 8 & 0xff) : v;
		return 0;
	}
	if (a >= TI_IO_END)		/* nothing there; AMS posts each key to $C00000 */
		v = 0;
	else
		for (i = 0; i < sz; i++)
			v = v << 8 | iorb(a + i);
	tic.st.io++;
	*vp = v;
	return 0;
}

int
ti_write(a, sz, v)
	unsigned long a, v;
	int sz;
{
	int i;

	if (a >= TI_FLASH && a < TI_FLASH + TI_FLASH_SIZE) {
		if (sz == 1)
			return fput(a & ~1UL, (int)(v << 8 | v));
		for (i = 0; i < sz; i += 2)
			if (fput(a + i, (int)(v >> 8 * (sz - 2 - i))))
				return -1;
		return 0;
	}
	tic.st.io++;
	for (i = 0; i < sz && a < TI_IO_END; i++)
		iowb(a + i, (int)(v >> 8 * (sz - 1 - i)) & 0xff);
	return 0;
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

int
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
	bzero((caddr_t)&tic.io, sizeof tic.io);
	bzero((caddr_t)&tic.seen, sizeof tic.seen);
	bzero((caddr_t)&tic.st, sizeof tic.st);
	tic.io[0][0x1a] = 0xff;
	tic.st.lcd = 0x4c00;
	tic.pend = tic.halt = tic.fmode = tic.nnote = tic.nexc = tic.sleeping = 0;
	tic.cmdblk = 0;
	tic.flags = te.flags;
	tic.proc = curproc;
	tic.gp = gp;
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
		d->gd_fn = v == 2 ? ti_fault : v == 8 ? priv : exc;
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
