/*
 * guestcore.c -- guest processes: the guest_proc record, its life
 * across exec, fork and exit, and dispatch of exceptions, signal
 * frames and address checks to the process's profile.  The virtual
 * CPU (A-line reflection, privileged instructions, the virtual IPL)
 * is in gcpu.c.
 *
 * A guest process holds its profile's module (gpf_wrapper) from
 * creation to free; profile modules depend on this one.
 *
 * K&R C.
 */

#include "kinc.h"

extern int guest_fsig(), guest_unote();

static struct guest_profile *gc_profiles;
static int gc_nproc;

int
guest_profile_add(pf)
	struct guest_profile *pf;
{
	pf->gpf_next = gc_profiles;
	gc_profiles = pf;
	return 0;
}

void
guest_profile_del(pf)
	struct guest_profile *pf;
{
	struct guest_profile **pp;

	for (pp = &gc_profiles; *pp; pp = &(*pp)->gpf_next)
		if (*pp == pf) {
			*pp = pf->gpf_next;
			break;
		}
}

static struct guest_proc *
gc_alloc(p, pf)
	struct proc *p;
	struct guest_profile *pf;
{
	struct guest_proc *gp;
	u_int n = sizeof (struct guest_proc) + pf->gpf_privsz;

	gp = (struct guest_proc *)kmem_zalloc(n, KM_SLEEP);
	gp->gp_proc = p;
	gp->gp_prof = pf;
	gp->gp_size = n;
	mod_hold(pf->gpf_wrapper);
	gc_nproc++;
	p->p_evpdp = (struct evpd *)gp;
	return gp;
}

static void
gc_free(p)
	struct proc *p;
{
	struct guest_proc *gp = GUESTP(p);
	struct guest_profile *pf = gp->gp_prof;

	if (pf->gpf_exit)
		(*pf->gpf_exit)(gp);
	p->p_evpdp = 0;
	kmem_free((_VOID *)gp, gp->gp_size);
	gc_nproc--;
	mod_rele(pf->gpf_wrapper);
}

/* a native process becomes a guest of pf (its launcher enters the ROM) */
int
guest_attach(pf)
	struct guest_profile *pf;
{
	if (curproc->p_evpdp)
		return EBUSY;
	(void)gc_alloc(curproc, pf);
	return 0;
}

/* The caller can detach only its own profile. */
int
guest_detach(pf)
	struct guest_profile *pf;
{
	if (!curproc->p_evpdp || GUESTP(curproc)->gp_prof != pf)
		return ENXIO;
	gc_free(curproc);
	return 0;
}

/* every exec of a guest, and the exec that guest_loading names */
static void
gc_exec(p)
	struct proc *p;
{
	struct guest_profile *pf = guest_loading == p ? guest_loadprof : 0;
	struct guest_proc *gp;

	if (p->p_evpdp && GUESTP(p)->gp_prof != pf) {
		GUESTP(p)->gp_flags |= GPF_EXEC;
		gc_free(p);
	}
	if (pf) {
		if (!p->p_evpdp)
			(void)gc_alloc(p, pf);
		gp = GUESTP(p);
		gp->gp_flags = 0;		/* a new image: no virtual CPU state */
		gp->gp_vsr = 0;
		gp->gp_vusp = gp->gp_vvbr = gp->gp_vcacr = 0;
		gp->gp_vsfc = gp->gp_vdfc = 0;
		if (pf->gpf_exec)
			(*pf->gpf_exec)(gp);
	}
}

static int
gc_fork(pp, cp)
	struct proc *pp, *cp;
{
	struct guest_proc *pg = GUESTP(pp), *cg;
	struct guest_profile *pf = pg->gp_prof;
	int e;

	cg = gc_alloc(cp, pf);
	cg->gp_flags = pg->gp_flags & ~GPF_VPEND;
	cg->gp_vsr = pg->gp_vsr;
	cg->gp_vusp = pg->gp_vusp;
	cg->gp_vvbr = pg->gp_vvbr;
	cg->gp_vcacr = pg->gp_vcacr;
	cg->gp_vsfc = pg->gp_vsfc;
	cg->gp_vdfc = pg->gp_vdfc;
	bcopy(GUEST_PRIV(pg), GUEST_PRIV(cg), pf->gpf_privsz);
	if (pf->gpf_fork && (e = (*pf->gpf_fork)(pg, cg)) != 0) {
		gc_free(cp);
		return e;
	}
	return 0;
}

static void
gc_exit(p, stat)
	struct proc *p;
	int stat;
{
	gc_free(p);
}

static int
gc_trap(r)
	char *r;
{
	struct guest_proc *gp = GUESTP(curproc);
	int v = GR_VEC(r);
	struct guest_disp *d = &gp->gp_prof->gpf_disp[v];

	if (d->gd_kind == GD_NATIVE || d->gd_fn == 0)
		return 1;
	if ((d->gd_flags & GDF_USER) && (GR_SR(r) & 0x2000))
		return 1;
	u.u_ar0 = (struct pcb *)r;	/* for sendsig from the handler's tail */
	return (*d->gd_fn)(gp, r, v);
}

static int
gc_sendsig(sig, sip, hdlr)
	int sig;
	char *sip;
	int (*hdlr)();
{
	struct guest_proc *gp = GUESTP(curproc);

	if (gp->gp_prof->gpf_sendsig)
		return (*gp->gp_prof->gpf_sendsig)(gp, sig, sip, hdlr);
	return __amix_sendsig(sig, sip, hdlr);
}

static int
gc_vur(a, len)
	caddr_t a;
	u_int len;
{
	struct guest_proc *gp = GUESTP(curproc);

	if (gp->gp_prof->gpf_vur)
		return (*gp->gp_prof->gpf_vur)(gp, a, len);
	return __amix_valid_usr_range(a, len);
}

static int
guestcore_unload()
{
	return gc_profiles || gc_nproc ? EBUSY : 0;
}

struct mod_hook_data guestcore_hookdata[] = {
	{ "guest_trap",		gc_trap },
	{ "guest_exec",		(int (*)())gc_exec },
	{ "guest_fork",		gc_fork },
	{ "guest_exit",		(int (*)())gc_exit },
	{ "guest_sendsig",	gc_sendsig },
	{ "guest_vur",		gc_vur },
	{ "guest_fsig",		guest_fsig },
	{ "guest_unote",	guest_unote },
	{ 0, 0 }
};

MOD_HOOK_WRAPPER(guestcore, 0, guestcore_unload, "guest process core");
