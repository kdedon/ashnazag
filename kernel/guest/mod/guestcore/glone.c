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

/* guest_reflect's n for a CPU frame with format word fv; 0: a format not passed on */
int
guest_fvsize(fv)
int fv;
{
	switch (fv >> 12 & 0xf) {
	case 0: return 2;
	case 2: case 3: return 6;
	case 4: return 10;
	}
	return 0;
}

/*
 * An exception through the guest's vector at fv's offset, with the rest
 * of the CPU's own frame, then back to the guest.  1: a format left to
 * the caller.  name: the console line when the guest has no stack.
 */
int
guest_reflcpu(gp, r, pc, fv, name)
struct guest_proc *gp;
char *r, *name;
long pc;
int fv;
{
	int n = guest_fvsize(fv);

	if (!n)
		return 1;
	if (guest_reflect(gp, r, pc, fv, r + 72, n, -1) < 0) {
		if (name)
			printf("%s: vector %d with no guest stack, pc %x\n", name, (fv & 0xfff) >> 2, (int)pc);
		psignal(curproc, SIGSEGV);
	}
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

static int
lone_reflect(gp, r, v, pc, format)
struct guest_proc *gp;
char *r;
int v, format;
long pc;
{
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
	if (gp->gp_flags & GPF_FRAME68K)
		format = 0;
	if (!guest_fvsize(format))
		return 1;
	guest_lone_stat.reflected++;
	return guest_reflcpu(gp, r, pc, format ? format : v << 2, (char *)0);
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
	/* an untraced user-mode A-line goes to the profile's entry with its pc pushed, staying in user mode */
	if (v == 10 && gp->gp_ualine && !(gp->gp_vsr & 0x2000) && !(GR_SR(r) & 0x8000)) {
		long pc = GR_PC(r);

		if (copyout((caddr_t)&pc, (caddr_t)(GR_USP(r) - 4), 4) == 0) {
			GR_USP(r) -= 4;
			GR_PC(r) = gp->gp_ualine;
			guest_trapret();
			return 0;
		}
	}
	if (v != 8 && v != 11)
		return lone_reflect(gp, r, v, GR_PC(r), GR_FV(r));
	if (!copyin((caddr_t)GR_PC(r), b, 2))
		op = G16(b);
	/* a 68000 reads SR in user mode too */
	if (v == 8 && !(gp->gp_vsr & 0x2000) && !((gp->gp_flags & GPF_FRAME68K) && (op & 0xffc0) == 0x40c0))
		return lone_reflect(gp, r, 8, GR_PC(r), GR_FV(r));
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
