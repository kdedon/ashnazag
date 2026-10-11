#include "sys/types.h"
#include "sys/conf.h"
#include "kinc.h"
#include "cpmio.h"

extern int nodev();
extern struct modwrapper cpmguest_wrapper;
static struct guest_profile cpm_profile;
static int cpm_nopen, cpm_flag;

static int
cpm_open(devp, flag, otyp, cr)
dev_t *devp;
int flag, otyp;
struct cred *cr;
{
	if (getminor(*devp))
		return ENXIO;
	cpm_nopen++;
	return 0;
}

static int
cpm_close(dev, flag, otyp, cr)
dev_t dev;
int flag, otyp;
struct cred *cr;
{
	cpm_nopen = 0;
	return 0;
}

static int
cpm_ioctl(dev, cmd, arg, mode, cr, rv)
dev_t dev;
int cmd, mode;
caddr_t arg;
struct cred *cr;
int *rv;
{
	struct guest_lone_stat st;

	if (cmd == CPMIOC_ENTER)
		return guest_lone_enter(&cpm_profile);
	if (cmd == CPMIOC_LEAVE)
		return guest_detach(&cpm_profile);
	if (cmd == CPMIOC_STAT) {
		guest_lone_stats(&st);
		return copyout((caddr_t)&st, arg, sizeof st) ? EFAULT : 0;
	}
	return EINVAL;
}

/* no guest interrupts to hold back: Ctrl-C and hangup reach startcpm at once */
static int
cpm_fsig(p, gp)
struct proc *p;
struct guest_proc *gp;
{
	return __amix_fsig(p);
}

static int
cpm_load()
{
	int e = guest_lone_init(&cpm_profile, "cpm", &cpmguest_wrapper);

	cpm_profile.gpf_fsig = cpm_fsig;
	return e;
}

static int
cpm_unload()
{
	if (cpm_nopen)
		return EBUSY;
	guest_profile_del(&cpm_profile);
	return 0;
}

struct mod_drv_data cpmguest_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
			{ cpm_open, cpm_close, nodev, nodev, cpm_ioctl, nodev, nodev, nodev,
					nodev, nodev, 0, 0, &cpm_flag }, CPM_MAJOR, 1 }
};
MOD_DRV_WRAPPER(cpmguest, cpm_load, cpm_unload, 0, "CP/M guest");
