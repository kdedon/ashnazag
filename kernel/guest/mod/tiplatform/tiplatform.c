#include "sys/types.h"
#include "sys/conf.h"
#include "kinc.h"
#include "profile.h"

extern int nodev(), ttimeout(), untimeout(), delay();
extern void dlm_cacheflush();
extern struct modwrapper tiplatform_wrapper;
static struct guest_profile profile;
static int opened, device_flag;
struct ti_state {
	struct proc *process;
	int timer, interval, masked;
	unsigned long idle, critical, rejected;
};
#define STATE(gp) ((struct ti_state *)GUEST_PRIV(gp))

static void
cancel(s)
struct ti_state *s;
{
	if (s->timer > 0)
		untimeout(s->timer);
	s->timer = 0;
	s->interval = 0;
}

static void
tick(arg)
caddr_t arg;
{
	struct ti_state *s = (struct ti_state *)arg;
	s->timer = 0;
	if (!s->process || !s->interval)
		return;
	psignal(s->process, SIGALRM);
	s->timer = ttimeout(tick, arg, (long)s->interval);
	if (s->timer == -1) {
		s->timer = 0;
		psignal(s->process, SIGTERM);
	}
}

static void
exited(gp)
struct guest_proc *gp;
{
	struct ti_state *s = STATE(gp);
	cancel(s);
	s->process = 0;
}

static int
forked(parent, child)
struct guest_proc *parent, *child;
{
	struct ti_state *s = STATE(child);
	s->process = child->gp_proc;
	s->timer = s->interval = 0;
	return 0;
}

static int
signals(p, gp)
struct proc *p;
struct guest_proc *gp;
{
	k_sigset_t saved = p->p_hold;
	int result;
	if (STATE(gp)->masked)
		p->p_hold |= sigmask(SIGALRM);
	result = __amix_fsig(p);
	p->p_hold = saved;
	return result;
}

static int
fault(gp, r, signal)
struct guest_proc *gp;
char *r;
int signal;
{
	STATE(gp)->rejected++;
	printf("NOTICE: Voyage pid %d signal %d pc %x sp %x d0 %x a0 %x\n",
		(int)curproc->p_pid, signal, (int)GR_PC(r), (int)GR_USP(r),
		(int)GR_D(r, 0), (int)GR_A(r, 0));
	psignal(curproc, signal);
	guest_trapret();
	return 0;
}

static int
trap(gp, r, vector)
struct guest_proc *gp;
char *r;
int vector;
{
	struct ti_state *s = STATE(gp);
	unsigned long pc = GR_PC(r), chain, sp, ret, next, target;
	unsigned short opcode, request = GR_D(r, 0), queue[4];
	unsigned char context[60], bytes[2];
	int previous;
	if (pc < TI_IMAGE || pc >= TI_IMAGE_END)
		return 1;
	if (vector == 33) {
		if (pc != TI_MASK_PC || (request != 0 && request != 1 && request != 0x400 && request != 0x500))
			return fault(gp, r, SIGILL);
		previous = s->masked;
		s->masked = request != 0;
		s->critical++;
		GR_D(r, 0) = previous;
	} else if (vector == 32) {
		if (pc != TI_SERVICE_PC)
			return fault(gp, r, SIGILL);
		if (request == 0x272) {
			if (s->idle) {
				sp = GR_USP(r);
				if ((sp & 1) || sp < TI_RAM + 4 || sp > TI_RAM_END - 4 ||
				    copyout((caddr_t)&pc, (caddr_t)(sp - 4), 4))
					return fault(gp, r, SIGILL);
				GR_USP(r) = sp - 4;
				GR_PC(r) = s->idle;
			} else
				delay(1L);
		} else if (request == 0x2e0 || request == 0x2f6) {
			if (copyin((caddr_t)0x5b9a, (caddr_t)bytes, 2))
				return fault(gp, r, SIGILL);
			if (request == 0x2e0) {
				bytes[0] = 0;
				if (copyout((caddr_t)bytes, (caddr_t)0x5b9a, 1))
					return fault(gp, r, SIGILL);
			} else {
				GR_D(r, 0) = bytes[1] ? 0 : bytes[0];
				if (GR_D(r, 0)) {
					queue[0] = queue[1] = queue[3] = 0;
					queue[2] = 2;
					if (copyout((caddr_t)queue, (caddr_t)TI_QUEUE, 8))
						return fault(gp, r, SIGILL);
				}
			}
		} else
			return fault(gp, r, SIGSYS);
	} else if (vector == 10) {
		if (copyin((caddr_t)pc, (caddr_t)&opcode, 2) || (opcode & 0xf000) != 0xa000 ||
		    copyin((caddr_t)TI_CHAIN, (caddr_t)&chain, 4) || (chain & 1) ||
		    chain < TI_RAM || chain > TI_RAM_END - 60 ||
		    copyin((caddr_t)chain, (caddr_t)context, 60))
			return fault(gp, r, SIGILL);
		sp = G32(context + 40); ret = G32(context + 52); next = G32(context + 56);
		if ((sp & 1) || sp < TI_RAM || sp > TI_RAM_END - 8 || (ret & 1) ||
		    ret < TI_IMAGE || ret >= TI_IMAGE_END || (next &&
		    ((next & 1) || next < TI_RAM || next > TI_RAM_END - 60)))
			return fault(gp, r, SIGILL);
		/* Probe writable context without changing the original unwind record. */
		if (copyout((caddr_t)context, (caddr_t)chain, 60))
			return fault(gp, r, SIGILL);
		target = opcode & 0xfff;
		GR_D(r, 0) = target == 0xfff ? -1 : target;
		GR_PC(r) = TI_UNWIND;
	} else
		return fault(gp, r, vector == 2 ? SIGBUS : vector == 3 ? SIGSEGV : SIGILL);
	guest_trapret();
	return 0;
}

static int
admit()
{
	unsigned char block[256];
	unsigned long address, crc = ~0UL;
	int i, bit;
	for (address = TI_RAM; address < TI_RAM_END; address += sizeof block)
		if (copyin((caddr_t)address, (caddr_t)block, sizeof block) ||
		    copyout((caddr_t)block, (caddr_t)address, sizeof block))
			return EFAULT;
	for (address = TI_IMAGE; address < TI_IMAGE_END; address += sizeof block) {
		if (copyin((caddr_t)address, (caddr_t)block, sizeof block))
			return EFAULT;
		for (i = 0; i < sizeof block; i++) {
			crc ^= block[i];
			for (bit = 0; bit < 8; bit++)
				crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
		}
	}
	return ~crc == TI_EXEC_CRC ? 0 : EINVAL;
}

static int
open_device(dev, flags, kind, credentials)
dev_t *dev;
int flags, kind;
struct cred *credentials;
{
	if (getminor(*dev)) return ENXIO;
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
	struct guest_proc *gp = GUESTP(curproc);
	struct ti_state *s;
	struct ti_info info;
	long value;
	int error;
	if (command == TI_ENTER) {
		if (gp) return EBUSY;
		if ((error = admit()) || (error = guest_attach(&profile))) return error;
		gp = GUESTP(curproc);
		gp->gp_flags = 0;
		STATE(gp)->process = curproc;
		dlm_cacheflush();
		return 0;
	}
	if (!gp || gp->gp_prof != &profile) return ENXIO;
	s = STATE(gp);
	if (command == TI_LEAVE) return guest_detach(&profile);
	if (command == TI_INFO) {
		bzero((caddr_t)&info, sizeof info);
		info.masked = s->masked; info.critical = s->critical; info.rejected = s->rejected;
		return copyout((caddr_t)&info, argument, sizeof info) ? EFAULT : 0;
	}
	if (command != TI_EVENTS && command != TI_IDLE) return EINVAL;
	if (copyin(argument, (caddr_t)&value, 4)) return EFAULT;
	if (command == TI_IDLE) {
		if (value && ((value & 1) || (unsigned long)value < 0x80000000UL ||
		    (unsigned long)value >= 0xc0000000UL ||
		    copyin((caddr_t)value, (caddr_t)&error, 2))) return EINVAL;
		s->idle = value;
		return 0;
	}
	if (value < 0 || value > HZ) return EINVAL;
	cancel(s);
	if (!value) return 0;
	s->interval = value;
	s->timer = ttimeout(tick, (caddr_t)s, value);
	if (s->timer == -1) { s->timer = s->interval = 0; return EAGAIN; }
	return 0;
}
static int
load()
{
	int v;
	bzero((caddr_t)&profile, sizeof profile);
	profile.gpf_name = "voyage200";
	profile.gpf_wrapper = &tiplatform_wrapper;
	profile.gpf_privsz = sizeof(struct ti_state);
	profile.gpf_exit = exited;
	profile.gpf_fork = forked;
	profile.gpf_fsig = signals;
	for (v = 2; v < 48; v++) {
		profile.gpf_disp[v].gd_kind = GD_SYSCALL;
		profile.gpf_disp[v].gd_flags = GDF_USER;
		profile.gpf_disp[v].gd_fn = trap;
	}
	return guest_profile_add(&profile);
}
static int
unload()
{
	if (opened) return EBUSY;
	guest_profile_del(&profile);
	return 0;
}
struct mod_drv_data tiplatform_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	{ open_device, close_device, nodev, nodev, ioctl_device, nodev, nodev,
		nodev, nodev, nodev, 0, 0, &device_flag }, TI_MAJOR, 1 }
};
MOD_DRV_WRAPPER(tiplatform, load, unload, 0, "Voyage native services");
