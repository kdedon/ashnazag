/* /dev/kstat as a loadable driver: loaded on the first open. */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sys/conf.h"
#include "sys/file.h"
#include "sys/uio.h"
#include "sys/kmem.h"
#include "sys/inline.h"
#include "sys/systm.h"
#include "sys/time.h"
#include "sys/proc.h"
#include "sys/sysinfo.h"
#include "sys/vmmeter.h"
#include "sys/moddefs.h"
#include "kstat.h"

/*
 * Load averages, sampled every 5 seconds.  The kernel's own leave out
 * the running process, so one busy process reads 0.00; these count it.
 * 11-bit fixed point; the decays are exp(-5/60), exp(-5/300), exp(-5/900).
 */
#define FSHIFT	11
#define FSCALE	(1 << FSHIFT)

#define _CONFIG_PAGESIZE	6
#define _CONFIG_CLK_TCK		7
extern int sysconfig();

extern long avenrun[3];		/* the kernel's, 8-bit fixed point */
static long ks_avenrun[3];
static long ks_cexp[3] = { 1884, 2014, 2037 };
static long ks_nrun;
static int ks_toid;
static long ks_hz, ks_pagesize;

static void
ks_loadav()
{
	register proc_t *p;
	register long n = 0;
	register int i;

	for (p = practive; p; p = p->p_next)
		if (p->p_stat == SRUN || p->p_stat == SONPROC || p->p_stat == SIDL)
			n++;
	ks_nrun = n;
	for (i = 0; i < 3; i++)
		ks_avenrun[i] = (ks_avenrun[i] * ks_cexp[i] +
		    n * FSCALE * (FSCALE - ks_cexp[i])) >> FSHIFT;
	ks_toid = timeout(ks_loadav, (caddr_t)0, 5 * ks_hz);
}

static kstat_named_t *
ks_put(k, name, v)
	kstat_named_t *k;
	char *name;
	unsigned long v;
{
	bzero(k->name, KSTAT_STRLEN);
	strncpy(k->name, name, KSTAT_STRLEN - 1);
	k->value = v;
	return k + 1;
}

#define NREC	48

static int
ks_snap(h)
	struct kstat_hdr *h;
{
	register kstat_named_t *k = (kstat_named_t *)(h + 1);
	int s;

	s = splhi();
	k = ks_put(k, "avenrun_1min", ks_avenrun[0]);
	k = ks_put(k, "avenrun_5min", ks_avenrun[1]);
	k = ks_put(k, "avenrun_15min", ks_avenrun[2]);
	k = ks_put(k, "fscale", FSCALE);
	k = ks_put(k, "nrun", ks_nrun);
	k = ks_put(k, "cpu_idle", sysinfo.cpu[CPU_IDLE]);
	k = ks_put(k, "cpu_user", sysinfo.cpu[CPU_USER]);
	k = ks_put(k, "cpu_kernel", sysinfo.cpu[CPU_KERNEL]);
	k = ks_put(k, "cpu_wait", sysinfo.cpu[CPU_WAIT]);
	k = ks_put(k, "wait_io", sysinfo.wait[W_IO]);
	k = ks_put(k, "wait_swap", sysinfo.wait[W_SWAP]);
	k = ks_put(k, "wait_pio", sysinfo.wait[W_PIO]);
	k = ks_put(k, "pagesize", ks_pagesize);
	k = ks_put(k, "physmem", physmem);
	k = ks_put(k, "freemem", freemem);
	k = ks_put(k, "availrmem", availrmem);
	k = ks_put(k, "availsmem", availsmem);
	k = ks_put(k, "pgin", sum.v_pgin);
	k = ks_put(k, "pgout", sum.v_pgout);
	k = ks_put(k, "pgpgin", sum.v_pgpgin);
	k = ks_put(k, "pgpgout", sum.v_pgpgout);
	k = ks_put(k, "pswpin", sum.v_pswpin);
	k = ks_put(k, "pswpout", sum.v_pswpout);
	k = ks_put(k, "faults", sum.v_faults);
	k = ks_put(k, "zfod", sum.v_zfod);
	k = ks_put(k, "pgrec", sum.v_pgrec);
	k = ks_put(k, "scan", sum.v_scan);
	k = ks_put(k, "dfree", sum.v_dfree);
	k = ks_put(k, "swtch", sum.v_swtch);
	k = ks_put(k, "intr", sum.v_intr);
	k = ks_put(k, "trap", sum.v_trap);
	k = ks_put(k, "syscall", sysinfo.syscall);
	k = ks_put(k, "sysfork", sysinfo.sysfork);
	k = ks_put(k, "sysexec", sysinfo.sysexec);
	k = ks_put(k, "readch", sysinfo.readch);
	k = ks_put(k, "writech", sysinfo.writech);
	k = ks_put(k, "boot_time", hrestime.tv_sec - lbolt / ks_hz);
	h->ks_snaptime = lbolt;
	splx(s);
	h->ks_magic = KSTAT_MAGIC;
	h->ks_hz = ks_hz;
	h->ks_ndata = k - (kstat_named_t *)(h + 1);
	return (char *)k - (char *)h;
}

/* ARGSUSED */
int
ksopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	return (flag & FWRITE) ? EACCES : 0;
}

/* ARGSUSED */
int
ksclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	return 0;
}

/* ARGSUSED */
int
ksread(dev, uiop, cr)
	dev_t dev;
	struct uio *uiop;
	struct cred *cr;
{
	struct kstat_hdr *h;
	int n, sz, e;

	sz = sizeof *h + NREC * sizeof(kstat_named_t);
	if ((h = (struct kstat_hdr *)kmem_alloc(sz, KM_SLEEP)) == 0)
		return ENOMEM;
	n = ks_snap(h);
	if (n > uiop->uio_resid)
		n = uiop->uio_resid;
	e = uiomove((caddr_t)h, n, UIO_READ, uiop);
	kmem_free((caddr_t)h, sz);
	return e;
}

static int ks_flag = D_NEW;

struct mod_drv_data kstat_drvdata[] = {
	{ { 0 }, 0, 0,
	  { ksopen, ksclose, ksread, nodev, nodev, 0, 0, 0,
	    0, 0, 0, 0, &ks_flag }, KSTAT_MAJOR, 1 }
};

/* the kernel's own page size and tick rate, which differ between builds */
static long
ks_config(which)
	int which;
{
	long rv[2];

	rv[0] = 0;
	sysconfig(&which, rv);
	return rv[0];
}

static int
kstat_load()
{
	register int i;

	if ((ks_hz = ks_config(_CONFIG_CLK_TCK)) <= 0)
		ks_hz = HZ;
	if ((ks_pagesize = ks_config(_CONFIG_PAGESIZE)) <= 0)
		ks_pagesize = PAGESIZE;

	for (i = 0; i < 3; i++)
		ks_avenrun[i] = avenrun[i] << (FSHIFT - 8);
	ks_toid = timeout(ks_loadav, (caddr_t)0, 5 * ks_hz);
	return 0;
}

static int
kstat_unload()
{
	untimeout(ks_toid);
	return 0;
}

MOD_DRV_WRAPPER(kstat, kstat_load, kstat_unload, 0, "system statistics");
