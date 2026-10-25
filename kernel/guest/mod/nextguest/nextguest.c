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

extern int nodev(), untimeout(), fpu_present;
extern void dlm_cacheflush();
extern struct modwrapper nextguest_wrapper;
extern void guest_priv_stats(), guest_priv_pcs();

struct guest_env nx_env;
static int device_flag;

/* the request at level l goes in through its autovector */
static int
nx_ack(es, r, l)
	struct guest_sess *es;
	char *r;
	int l;
{
	nx_post(l);
	return 24 + l;
}

/* the guest's space spans quadrants 0 to 2, which the stock rule refuses */
static int
nx_vur(gp, a, len)
	struct guest_proc *gp;
	caddr_t a;
	u_int len;
{
	u_long s = (u_long)a;

	if (s < NX_TOP && len <= NX_TOP - s)
		return len != 0;
	return __amix_valid_usr_range(a, len);
}

/* an exception through the guest's own vector */
static int
refl(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	/* the host's own code; trap #0 there is a host system call */
	if ((unsigned long)GR_PC(r) >= NX_TOP && ((nx.flags & NXF_HOST) || v == 32))
		return 1;
	if (!guest_fvsize(GR_FV(r)))
		return 1;
	return guest_reflcpu(gp, r, GR_PC(r), (GR_FV(r) & 0xf000) | (v << 2), (char *)0);
}

/* a host call from the kernel's patched routines; anything else illegal is the guest's */
static int
nx_hostcall(es, r, n)
	struct guest_sess *es;
	char *r;
	int n;
{
	int s, e;

	if (!(es->gs_gp->gp_vsr & SR_S) || (e = nx_service(es->gs_gp, r)) < 0)
		return -1;
	s = guest_splhi();
	es->gs_gp->gp_vpend = nx_level() << 8;
	guest_splx(s);
	return e;
}

/* reset: the peripherals, not memory or the clock chip */
static void
nx_reset(gp, r)
	struct guest_proc *gp;
	char *r;
{
	int s = guest_splhi();

	nx.stat = nx.istat = 0;
	if (nx.tid > 0)
		untimeout(nx.tid);
	nx.tid = 0;
	guest_splx(s);
}

/* vectors 8 and 11: what guestcore leaves is the guest's */
static int
priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int e;

	if ((e = guest_privtrap(gp, r, v)) <= 1)
		return e;
	return refl(gp, r, e);
}

/* data types the FPU leaves to software, such as packed decimal, go to the host's FP package */
static int
fpdata(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	return fpu_present ? 1 : refl(gp, r, v);
}

static void
nx_exit(es)
	struct guest_sess *es;
{
	if (nx.tid > 0)
		untimeout(nx.tid);
	nx.tid = 0;
	guest_ramdev_free(&nx_env.ge_ram);
	sigdelset(&es->gs_proc->p_sig, NX_SIG);
}

/* ---- the device ---- */

static int
enter(es, arg)
	struct guest_sess *es;
	caddr_t arg;
{
	struct nx_enter ne;
	struct guest_proc *gp = es->gs_gp;

	if (copyin(arg, (caddr_t)&ne, sizeof ne))
		return EFAULT;
	if (ne.width < 16 || ne.width > 4092 || ne.height < 16 || ne.height > 4095 ||
	    ne.width / 4 * ne.height > NX_VRAM_SIZE)
		return EINVAL;
	bzero((caddr_t)&nx.st, sizeof nx.st);
	bcopy((caddr_t)ne.nvram, (caddr_t)nx.st.nvram, NX_NVRAM);
	nx.lastu = ~0UL;
	nx.ramsize = ne.ramsize;
	nx.flags = ne.flags;
	nx.stat = nx.istat = 0;
	nx.tid = 0;
	nx_board(ne.width, ne.height);
	/* the launcher above the guest's space finishes before an interrupt; its privileged instructions are emulated */
	if (ne.flags & NXF_HOST) {
		gp->gp_holdlo = NX_TOP;
		gp->gp_holdhi = ~0UL;
	}
	if (nx_env.ge_ram.rd_ram && guest_mmu_init(gp, NX_PHYS, NX_LIMIT, 0UL, NX_TOP))
		printf("NOTICE: next: no MMU shadow on this host\n");
	else if (ne.flags & NXF_PMAP)
		(void)guest_mmu_svc(gp, 1, 0UL, 0UL, 0UL, 0UL);
	dlm_cacheflush();
	return 0;
}

static int
mmap_device(dev, off, prot)
	dev_t dev;
	off_t off;
	int prot;
{
	return guest_env_mmap(&nx_env, dev, off, prot);
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
	return guest_env_segmap(&nx_env, dev, off, as, addrp, len, prot, maxprot, flags, cr);
}

static int
open_device(dev, flags, kind, credentials)
	dev_t *dev;
	int flags, kind;
	struct cred *credentials;
{
	return guest_env_open(&nx_env, dev, flags, kind, credentials);
}

static int
close_device(dev, flags, kind, credentials)
	dev_t dev;
	int flags, kind;
	struct cred *credentials;
{
	return guest_env_close(&nx_env, dev, flags, kind, credentials);
}

static int
ioctl_device(dev, command, argument, mode, credentials, result)
	dev_t dev;
	int command, mode;
	caddr_t argument;
	struct cred *credentials;
	int *result;
{
	return guest_env_ioctl(&nx_env, dev, command, argument, mode, credentials, result);
}

static int
nx_ioctl(e, es, command, argument, mode, result)
	struct guest_env *e;
	struct guest_sess *es;
	int command, mode;
	caddr_t argument;
	int *result;
{
	unsigned long size;
	struct nx_mmu q;
	int s;

	switch (command) {
	case NX_MMU:
		if (es->gs_state != GS_RUN || curproc != es->gs_proc)
			return ENXIO;
		nx.st.hcalls[0]++;
		return copyin(argument, (caddr_t)&q, sizeof q) ? EFAULT :
		    guest_mmu_svc(es->gs_gp, (int)q.op, q.key, q.start, q.end, q.pa);
	case NX_RAMNEW:
		if (es->gs_state != GS_FREE || GUESTP(curproc))
			return EBUSY;
		return copyin(argument, (caddr_t)&size, sizeof size) ? EFAULT :
		    guest_ramdev_new(&e->ge_ram, size, 0, 0UL);
	case NX_INFO:
		if (es->gs_state != GS_RUN)
			return ENXIO;
		guest_mmu_stats(es->gs_gp, nx.st.mmu);
		guest_priv_stats(nx.st.privk);
		guest_priv_pcs(nx.st.privpc);
		s = guest_splhi();
		nx.st.ints = es->gs_ints;
		nx.st.level = nx_level();
		nx.st.stat = nx.stat;
		guest_splx(s);
		return copyout((caddr_t)&nx.st, argument, sizeof nx.st) ? EFAULT : 0;
	}
	return EINVAL;
}

static int
load()
{
	struct guest_env *e = &nx_env;
	struct guest_profile *pf = &e->ge_prof;
	struct guest_disp *d;
	int v;

	bzero((caddr_t)e, sizeof *e);
	e->ge_name = "next";
	e->ge_wrapper = &nextguest_wrapper;
	e->ge_major = NX_MAJOR;
	e->ge_entercmd = NX_ENTER;
	e->ge_gpflags = GPF_PRIV | GPF_FTRAP | GPF_FTRAP13 | GPF_ALINE;
	e->ge_nsess = 1;
	e->ge_sig = NX_SIG;
	e->ge_flags = GEF_ONE | GEF_SIGTAIL | GEF_ONSTACK | GEF_HOSTHOLD;
	e->ge_hostva = HF_HCALL + 4;
	e->ge_hostn = 1;
	e->ge_hoststep = 2;
	e->ge_hostvec = 4;
	e->ge_hostlen = 2;
	e->ge_enter = enter;
	e->ge_exit = nx_exit;
	e->ge_level = nx_level;
	e->ge_ack = nx_ack;
	e->ge_hostcall = nx_hostcall;
	e->ge_ioctl = nx_ioctl;
	pf->gpf_vur = nx_vur;
	pf->gpf_reset = nx_reset;
	pf->gpf_privf = GPR_ISPSHADOW;
	for (v = 2; v < 48; v++) {
		if (v > 11 && v < 32)
			continue;
		d = &pf->gpf_disp[v];
		d->gd_kind = GD_EMULATE;
		d->gd_flags = GDF_USER;
		d->gd_fn = v == 2 ? nx_fault : v == 8 || v == 11 ? priv : refl;
	}
	d = &pf->gpf_disp[55];
	d->gd_kind = GD_EMULATE;
	d->gd_flags = GDF_USER;
	d->gd_fn = fpdata;
	return guest_env_add(e);
}

static int
unload()
{
	return guest_env_del(&nx_env);
}

struct mod_drv_data nextguest_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	{ open_device, close_device, nodev, nodev, ioctl_device, mmap_device, segmap_device,
		nodev, nodev, nodev, 0, 0, &device_flag }, NX_MAJOR, 1 }
};
MOD_DRV_WRAPPER(nextguest, load, unload, 0, "NeXT native services");
