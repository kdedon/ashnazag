#include "kinc.h"
#include "sys/sysm68k.h"
#include "guest_lone.h"

extern int fpu_present;
static struct guest_lone_stat guest_lone_stat;

void
guest_lone_stats(st)
struct guest_lone_stat *st;
{
	bcopy((caddr_t)&guest_lone_stat, (caddr_t)st, sizeof *st);
}

static int
lone_reflect(gp, r, v, pc, format)
struct guest_proc *gp;
char *r;
int v, format;
long pc;
{
	int n;
	/* Negative format requests a synthetic format-0 frame. */
	if (!fuword((caddr_t)(gp->gp_vvbr + (v << 2)))) {
		if (format < 0)
			return 1;
		if (v < 10 || v == 32 || (v > 11 && v < 32))
			return 1;
		if (fuword((caddr_t)(gp->gp_vvbr + 16)))
			return lone_reflect(gp, r, 4, v > 32 ? pc - 2 : pc, -1);
		guest_lone_stat.sigill++;
		psignal(curproc, SIGILL);
		guest_trapret();
		return 0;
	}
	if (format < 0)
		format = 0;
	switch (format >> 12) {
	case 0: n = 0; break;
	case 2: case 3: n = 4; break;
	case 4: n = 8; break;
	default: return 1;
	}
	guest_lone_stat.reflected++;
	if (guest_reflect(gp, r, pc, format ? format : v << 2,
					format ? r + 72 : (char *)0, n + 2, -1) < 0)
		psignal(curproc, SIGSEGV);
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

int
guest_lone_trap(gp, r, v)
struct guest_proc *gp;
char *r;
int v;
{
	char b[2];
	int op = -1;
	guest_lone_stat.traps++;
	if (v == 2)
		return 1;
	if (v == 32) {
		guest_lone_stat.hostcalls++;
		return 1;
	}
	if (v == 8 && !(gp->gp_vsr & 0x2000))
		return lone_reflect(gp, r, 8, GR_PC(r), GR_FV(r));
	if (v != 8 && v != 11)
		return lone_reflect(gp, r, v, GR_PC(r), GR_FV(r));
	if (!copyin((caddr_t)GR_PC(r), b, 2))
		op = G16(b);
	if (v == 11 || (op & 0xffc0) == 0xf000) {
		if (((op >> 9) & 7) == 1 && fpu_present)
			return 1;
		return lone_reflect(gp, r, v, GR_PC(r), GR_FV(r));
	}
	if (!fpu_present && (op & 0xfe00) == 0xf200 && (op >> 7 & 3) == 2)
		return lone_reflect(gp, r, 11, GR_PC(r), -1);
	guest_lone_stat.privileged++;
	if (op == 0x4e70) {
		guest_lone_stat.resets++;
		GR_PC(r) += 2;
		guest_trapret();
		return 0;
	}
	if (op == 0x4e72 && !copyin((caddr_t)GR_PC(r) + 2, b, 2)) {
		guest_lone_stat.stops++;
		guest_setsr(gp, r, G16(b));
		GR_PC(r) += 4;
		guest_trapret();
		return 0;
	}
	if (!guest_priv(gp, r, v))
		return 0;
	return lone_reflect(gp, r, 4, GR_PC(r), -1);
}

int
guest_lone_init(pf, name, wrapper)
struct guest_profile *pf;
char *name;
struct modwrapper *wrapper;
{
	int v;
	struct guest_disp *d;
	bzero((caddr_t)pf, sizeof *pf);
	pf->gpf_name = name;
	pf->gpf_wrapper = wrapper;
	for (v = 2; v < 48; v++) {
		if (v > 11 && v < 32)
			continue;
		d = &pf->gpf_disp[v];
		d->gd_flags = GDF_USER;
		d->gd_kind = v == 2 || v == 8 || v == 11 ? GD_EMULATE : v == 32 ? GD_SYSCALL : GD_REFLECT;
		d->gd_fn = guest_lone_trap;
	}
	return guest_profile_add(pf);
}

int
guest_lone_enter(pf)
struct guest_profile *pf;
{
	struct guest_proc *gp;
	int e;
	if ((e = guest_attach(pf)) != 0)
		return e;
	gp = GUESTP(curproc);
	gp->gp_flags |= GPF_PRIV | GPF_FTRAP;
	gp->gp_vsr = 0x2700;
	gp->gp_vpend = 0;
	gp->gp_vusp = gp->gp_vvbr = gp->gp_vcacr = 0;
	guest_lone_stat.entered++;
	return 0;
}
