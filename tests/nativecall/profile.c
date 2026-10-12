#include "sys/types.h"
#include "sys/conf.h"
#include "kinc.h"
#include "guest_native.h"
#include "fixture.h"

extern struct modwrapper nhcall_wrapper;
static struct guest_profile profile;
static struct ncf_stats counts;
static int opened, device_flag;

static struct guest_native *
state(gp)
struct guest_proc *gp;
{
	return (struct guest_native *)GUEST_PRIV(gp);
}

static int
control(gp, r, vector)
struct guest_proc *gp;
char *r;
int vector;
{
	if (vector == GNC_VECTOR) {
		if (guest_native_control(gp, state(gp), r))
			GR_D(r, 0) = EINVAL;
	} else {
		counts.private_traps++;
		GR_D(r, 0) = NCF_VALUE;
	}
	guest_trapret();
	return 0;
}

static void
interrupt(gp, r)
struct guest_proc *gp;
char *r;
{
	counts.private_intr++;
	if (gp->gp_flags & GPF_NCALL)
		counts.bad_intr++;
}

static int
signal_frame(gp, sig, info, handler)
struct guest_proc *gp;
int sig;
char *info;
int (*handler)();
{
	if (gp->gp_flags & GPF_NCALL)
		counts.bad_signal++;
	return __amix_sendsig(sig, info, handler);
}

static int
private_range(gp, address, size)
struct guest_proc *gp;
caddr_t address;
u_int size;
{
	counts.private_vur++;
	return 0;
}

static int
forked(parent, child)
struct guest_proc *parent, *child;
{
	counts.forks++;
	return guest_native_fork(parent, child, state(parent), state(child));
}

static void
exited(gp)
struct guest_proc *gp;
{
	counts.exits++;
	guest_native_exit(gp, state(gp));
}

static int
open_device(dev, flags, kind, credentials)
dev_t *dev;
int flags, kind;
struct cred *credentials;
{
	if (getminor(*dev))
		return ENXIO;
	opened++;
	return 0;
}

static int
close_device(dev, flags, kind, credentials)
dev_t dev;
int flags, kind;
struct cred *credentials;
{
	opened--;
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
	struct guest_proc *gp;
	int error;
	if (command == NCF_ATTACH) {
		if ((error = guest_attach(&profile)) != 0)
			return error;
		gp = GUESTP(curproc);
		gp->gp_flags = GPF_FTRAP;
		if ((error = guest_native_init(gp, state(gp))) != 0)
			guest_detach(&profile);
		return error;
	}
	if (command == NCF_DETACH)
		return guest_detach(&profile);
	if (command == NCF_STATS)
		return copyout((caddr_t)&counts, argument, sizeof counts) ? EFAULT : 0;
	return EINVAL;
}

static int
load()
{
	bzero((caddr_t)&profile, sizeof profile);
	profile.gpf_name = "nativecall-fixture";
	profile.gpf_wrapper = &nhcall_wrapper;
	profile.gpf_privsz = sizeof(struct guest_native);
	profile.gpf_disp[32].gd_kind = GD_SYSCALL;
	profile.gpf_disp[32].gd_flags = GDF_USER;
	profile.gpf_disp[32].gd_fn = control;
	profile.gpf_disp[GNC_VECTOR] = profile.gpf_disp[32];
	profile.gpf_fork = forked;
	profile.gpf_exit = exited;
	profile.gpf_intr = interrupt;
	profile.gpf_sendsig = signal_frame;
	profile.gpf_vur = private_range;
	return guest_profile_add(&profile);
}

static int
unload()
{
	if (opened)
		return EBUSY;
	guest_profile_del(&profile);
	return 0;
}

static int unused() { return ENXIO; }
struct mod_drv_data nhcall_drvdata[] = {
	{ { 0 }, 0, 0,
	{ open_device, close_device, unused, unused, ioctl_device, unused, unused,
		unused, unused, unused, 0, 0, &device_flag }, 63, 1 }
};
MOD_DRV_WRAPPER(nhcall, load, unload, 0, "native call fixture");
