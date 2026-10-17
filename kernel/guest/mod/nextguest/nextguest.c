/*
 * nextguest.c -- /dev/nextguest and the NeXTstation Turbo profile.
 *
 * The user's kernel runs natively in user mode, with the supervisor state
 * virtual (guestcore).  Its routines that drive the board's timer, clock
 * chip and software interrupts call host services instead (nextdev.c).
 * NX_SIG carries the interrupts, held while the guest's IPL masks them.
 *
 *	tick or host call --> nx.stat --> NX_SIG --> frame --> handler --> rte
 *
 * K&R C.
 */

#include "next.h"
#include "../../next/hfswire.h"
#include "sys/mman.h"
#include "vm/as.h"

extern int nodev(), ttimeout(), untimeout(), fpu_present;
extern void dlm_cacheflush();
extern int __amix_sendsig(), __amix_fsig();
extern struct modwrapper nextguest_wrapper;

static struct guest_profile profile;
static int device_flag;

int
nx_splhi()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}

void
nx_splx(s)
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

/* a request above the guest's IPL wakes a halted guest or sends the carrier; under spl */
void
nx_kick()
{
	int l;

	if (nx.state != 1)
		return;
	l = nx_level();
	nx.gp->gp_vpend = l << 8;
	if (l <= vipl(nx.gp))
		return;
	if (nx.sleeping)
		wakeup((caddr_t)&nx.sleeping);
	else
		psignal(nx.proc, NX_SIG);
}

/* a host signal handler runs on the alternate stack: not the guest's code */
static int
onstack(p)
	struct proc *p;
{
	return p == u.u_procp && (u.u_sigaltstack.ss_flags & SS_ONSTACK);
}

/*
 * Whether an interrupt may go to the frame r returns to; nonzero when it
 * may.  A sigreturn rebuilds the frame saved in u_sigsave over r, which
 * would undo the interrupt's PC and SR but keep its stack: once that frame
 * has nothing left to finish, r returns in its place.
 */
static int
tail(r)
	char *r;
{
	char *f = (char *)u.u_sigsave;
	int fmt = G16(f + 6) >> 12;

	if (onstack(u.u_procp))
		return 0;
	if (!(u.u_sigflag & USTKRESTORE))
		return 1;
	if (fmt == 7 ? (G16(f + 12) & 0xf000) || ((G16(f + 14) | G16(f + 16) | G16(f + 18)) & 0x80) :
	    fmt != 0 && fmt != 2 && fmt != 4)
		return 0;
	GR_SR(r) = G16(f);
	GR_PC(r) = G32(f + 2);
	u.u_sigflag &= ~USTKRESTORE;
	return 1;
}

static void
nx_intr(gp, r)
	struct guest_proc *gp;
	char *r;
{
	int s = nx_splhi(), l = nx_level();

	if (nx.gp != gp || nx.state != 1) {
		nx_splx(s);
		return;
	}
	gp->gp_vpend = l << 8;
	nx_splx(s);
	/* the frame goes to guest memory, which may fault */
	if (l > vipl(gp) && tail(r)) {
		nx_post(l);
		if (guest_reflect(gp, r, GR_PC(r), (24 + l) << 2, (char *)0, 2, l) < 0)
			psignal(curproc, SIGSEGV);
		else {
			nx.st.ints++;
			if (GR_FV(r) >> 12)
				u.u_sigflag |= USTKCLEAR;
		}
	}
	s = nx_splhi();
	l = nx_level();
	gp->gp_vpend = l << 8;
	if (l <= vipl(gp))
		sigdelset(&gp->gp_proc->p_sig, NX_SIG);
	nx_splx(s);
}

/* the carrier waits while the guest's IPL masks every request */
static int
nx_fsig(p, gp)
	struct proc *p;
	struct guest_proc *gp;
{
	k_sigset_t h;
	int s = nx_splhi(), n;

	h = p->p_hold;
	sigdelset(&p->p_hold, NX_SIG);
	if (nx.gp != gp || nx_level() <= vipl(gp) || onstack(p))
		sigaddset(&p->p_hold, NX_SIG);
	n = __amix_fsig(p);
	p->p_hold = h;
	nx_splx(s);
	return n;
}

static int
nx_sendsig(gp, sig, sip, hdlr)
	struct guest_proc *gp;
	int sig;
	char *sip;
	int (*hdlr)();
{
	if (sig != NX_SIG)
		return __amix_sendsig(sig, sip, hdlr);
	nx_intr(gp, (char *)u.u_ar0);
	return 1;
}

/* an exception through the guest's own vector */
static int
refl(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int n;

	/* the host's own code; trap #0 there is a host system call */
	if ((unsigned long)GR_PC(r) >= NX_TOP && ((nx.flags & NXF_HOST) || v == 32))
		return 1;
	switch (GR_FV(r) >> 12) {
	case 0: n = 0; break;
	case 2: case 3: n = 4; break;
	case 4: n = 8; break;
	default: return 1;
	}
	if (v >= 32 && !(gp->gp_vsr & 0x2000)) {
		unsigned long *t = nx.st.utrap[nx.st.utraps++ % 16];
		long fp;
		int i;

		t[0] = v;
		t[1] = GR_D(r, 0);
		t[2] = GR_PC(r);
		guest_mmu_creg(gp, 0x806, (long *)&t[3], 0);
		t[4] = GR_D(r, 5);
		fp = GR_A(r, 6);
		for (i = 5; i < 8; i++)
			if (copyin((caddr_t)fp + 4, (caddr_t)&t[i], 4) || copyin((caddr_t)fp, (caddr_t)&fp, 4))
				t[i] = 0, fp = 0;
	}
	if (guest_reflect(gp, r, GR_PC(r), (GR_FV(r) & 0xf000) | (v << 2), r + 72, n + 2, -1) < 0)
		psignal(curproc, SIGSEGV);
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

/* a host call from the kernel's patched routines; anything else illegal is the guest's */
static int
hcall(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int s;

	if ((unsigned long)GR_PC(r) != HF_HCALL + 4 || !(gp->gp_vsr & SR_S) || nx_service(gp, r) < 0)
		return refl(gp, r, v);
	GR_PC(r) += 2;
	s = nx_splhi();
	gp->gp_vpend = nx_level() << 8;
	nx_splx(s);
	guest_trapret();
	return 0;
}

static int
priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned char b[4];
	unsigned long *cp, *rp, value;
	int op, ext, c, s;

	if (!(gp->gp_vsr & SR_S))
		return refl(gp, r, v);
	if (copyin((caddr_t)GR_PC(r), (caddr_t)b, 2))
		return 1;
	op = G16(b);
	if (op == 0x4e70) {			/* reset: the peripherals, not memory or the clock chip */
		s = nx_splhi();
		nx.stat = nx.istat = 0;
		if (nx.tid > 0)
			untimeout(nx.tid);
		nx.tid = 0;
		nx_splx(s);
		GR_PC(r) += 2;
		guest_trapret();
		return 0;
	}
	if (op == 0x4e72) {			/* stop */
		if (copyin((caddr_t)GR_PC(r) + 2, (caddr_t)b, 2))
			return 1;
		guest_setsr(gp, r, (int)G16(b));
		GR_PC(r) += 4;
		s = nx_splhi();
		while (nx.state == 1 && nx_level() <= vipl(gp)) {
			nx.sleeping = 1;
			if (sleep((caddr_t)&nx.sleeping, (PZERO + 1) | PCATCH))
				break;
		}
		nx.sleeping = 0;
		gp->gp_flags |= GPF_IDROP;
		nx_splx(s);
		guest_trapret();
		return 0;
	}
	if ((op & 0xfffe) == 0x4e7a) {		/* movec: the MMU registers are the guest's to read back */
		if (copyin((caddr_t)GR_PC(r) + 2, (caddr_t)b, 2))
			return 1;
		ext = G16(b);
		c = ext & 0xfff;
		cp = 0;
		if (c == 2)
			cp = &gp->gp_vcacr;
		if (c >= 3 && c <= 7 && !gp->gp_mmu)
			cp = &nx.mmu[c - 3];
		if ((c == 0x805 || c == 0x806 || c == 0x807) && !gp->gp_mmu)
			cp = &nx.mmu[c - 0x800];
		if (c == 0x803 || c == 0x804)
			cp = &nx.mmu[c - 0x7fb];
		if (cp) {
			rp = ext >> 12 == 15 ? (unsigned long *)&GR_USP(r) : (unsigned long *)(r + 4 + (ext >> 12) * 4);
			if (op & 1) {
				value = *rp;
				if (c == 2) {
					value &= 0x80008000UL;
					dlm_cacheflush();
				}
				*cp = value;
			} else
				*rp = *cp;
			GR_PC(r) += 4;
			guest_trapret();
			return 0;
		}
	}
	if (guest_priv(gp, r, v) == 0)
		return 0;
	return refl(gp, r, 4);
}

/* floating point goes to the host; anything else in line F is the guest's */
static int
fline(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned char b[2];

	if (copyin((caddr_t)GR_PC(r), (caddr_t)b, 2) == 0 && ((G16(b) >> 9) & 7) == 1 && fpu_present)
		return 1;
	return refl(gp, r, v);
}

static void
nx_exit(gp)
	struct guest_proc *gp;
{
	if (nx.gp != gp)
		return;
	if (nx.tid > 0)
		untimeout(nx.tid);
	nx.tid = 0;
	nx.state = 0;
	nx.proc = 0;
	nx.gp = 0;
	if (nx.ram)
		guest_ram_free(nx.ram);
	nx.ram = 0;
	sigdelset(&gp->gp_proc->p_sig, NX_SIG);
	if (nx.sleeping)
		wakeup((caddr_t)&nx.sleeping);
}

static int
nx_fork(parent, child)
	struct guest_proc *parent, *child;
{
	return EINVAL;
}

/* ---- the device ---- */

static int
enter(arg)
	caddr_t arg;
{
	struct nx_enter ne;
	struct guest_proc *gp;
	int e, s;

	if (copyin(arg, (caddr_t)&ne, sizeof ne))
		return EFAULT;
	if (ne.width < 16 || ne.width > 4092 || ne.height < 16 || ne.height > 4095 ||
	    ne.width / 4 * ne.height > NX_VRAM_SIZE)
		return EINVAL;
	s = nx_splhi();
	if (nx.state) {
		nx_splx(s);
		return EBUSY;
	}
	nx.state = 2;
	nx_splx(s);
	if ((e = guest_attach(&profile)) != 0) {
		nx.state = 0;
		return e;
	}
	gp = GUESTP(curproc);
	gp->gp_vsr = 0x2700;
	gp->gp_vpend = 0;
	gp->gp_vusp = gp->gp_vvbr = gp->gp_vcacr = 0;
	gp->gp_flags = GPF_PRIV | GPF_FTRAP | GPF_FTRAP13 | GPF_ALINE;
	bzero((caddr_t)&nx.st, sizeof nx.st);
	bcopy((caddr_t)ne.nvram, (caddr_t)nx.st.nvram, NX_NVRAM);
	bzero((caddr_t)nx.mmu, sizeof nx.mmu);
	nx.ramsize = ne.ramsize;
	nx.flags = ne.flags;
	nx.sleeping = 0;
	nx.stat = nx.istat = 0;
	nx.tid = 0;
	nx_board(ne.width, ne.height);
	if (nx.ram && guest_mmu_init(gp, NX_PHYS, NX_LIMIT, 0UL, NX_TOP))
		printf("NOTICE: next: no MMU shadow on this host\n");
	nx.proc = curproc;
	nx.gp = gp;
	dlm_cacheflush();
	nx.state = 1;
	return 0;
}

/* the RAM object, for the process that made it */
static int
mmap_device(dev, off, prot)
	dev_t dev;
	off_t off;
	int prot;
{
	return off < 0 ? -1 : guest_ram_frame(nx.ram, (unsigned long)off);
}

static int
segmap_device(dev, off, as, addrp, len, prot, maxprot, flags, cr)
	dev_t dev;
	off_t off;
	struct as *as;
	addr_t *addrp;
	u_int len, prot, maxprot, flags;
	struct cred *cr;
{
	if (!nx.ram || curproc->p_pid != nx.rampid)
		return EACCES;
	return guest_ram_map(nx.ram, dev, off, as, addrp, len, prot, maxprot, flags);
}

static int
open_device(dev, flags, kind, credentials)
	dev_t *dev;
	int flags, kind;
	struct cred *credentials;
{
	if (getminor(*dev))
		return ENXIO;
	nx.opened++;
	return 0;
}

static int
close_device(dev, flags, kind, credentials)
	dev_t dev;
	int flags, kind;
	struct cred *credentials;
{
	nx.opened = 0;
	if (!nx.state && nx.ram) {
		guest_ram_free(nx.ram);
		nx.ram = 0;
	}
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
	unsigned long size;
	int s;

	switch (command) {
	case NX_ENTER:
		return GUESTP(curproc) ? EBUSY : enter(argument);
	case NX_RAMNEW:
		if (nx.state || GUESTP(curproc))
			return EBUSY;
		if (nx.ram)
			guest_ram_free(nx.ram);
		nx.ram = 0;
		nx.rampid = curproc->p_pid;
		return copyin(argument, (caddr_t)&size, sizeof size) ? EFAULT :
		    guest_ram_new(size, 0, 0UL, &nx.ram);
	case NX_INFO:
		if (nx.state != 1)
			return ENXIO;
		guest_mmu_stats(nx.gp, nx.st.mmu);
		s = nx_splhi();
		nx.st.level = nx_level();
		nx.st.stat = nx.stat;
		nx_splx(s);
		return copyout((caddr_t)&nx.st, argument, sizeof nx.st) ? EFAULT : 0;
	}
	return EINVAL;
}

static int
load()
{
	struct guest_disp *d;
	int v;

	bzero((caddr_t)&profile, sizeof profile);
	profile.gpf_name = "next";
	profile.gpf_wrapper = &nextguest_wrapper;
	profile.gpf_exit = nx_exit;
	profile.gpf_fork = nx_fork;
	profile.gpf_fsig = nx_fsig;
	profile.gpf_sendsig = nx_sendsig;
	profile.gpf_intr = nx_intr;
	for (v = 2; v < 48; v++) {
		if (v > 11 && v < 32)
			continue;
		d = &profile.gpf_disp[v];
		d->gd_kind = GD_EMULATE;
		d->gd_flags = GDF_USER;
		d->gd_fn = v == 2 ? nx_fault : v == 4 ? hcall : v == 8 ? priv : v == 11 ? fline : refl;
	}
	d = &profile.gpf_disp[55];
	d->gd_kind = GD_EMULATE;
	d->gd_flags = GDF_USER;
	d->gd_fn = refl;
	return guest_profile_add(&profile);
}

static int
unload()
{
	if (nx.state || nx.opened)
		return EBUSY;
	guest_profile_del(&profile);
	return 0;
}

struct mod_drv_data nextguest_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	{ open_device, close_device, nodev, nodev, ioctl_device, mmap_device, segmap_device,
		nodev, nodev, nodev, 0, 0, &device_flag }, NX_MAJOR, 1 }
};
MOD_DRV_WRAPPER(nextguest, load, unload, 0, "NeXT native services");
