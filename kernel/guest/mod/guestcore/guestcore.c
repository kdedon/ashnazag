/*
 * guestcore.c -- guest processes: the guest_proc record, its life
 * across exec, fork and exit, and dispatch of exceptions, signal
 * frames and address checks to the process's profile.
 *
 * A guest process holds its profile's module (gpf_wrapper) from
 * creation to free; profile modules depend on this one.
 *
 * K&R C.
 */

#include "kinc.h"

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

/* every exec of a guest, and the exec that guest_loading names */
static void
gc_exec(p)
	struct proc *p;
{
	struct guest_profile *pf = guest_loading == p ? guest_loadprof : 0;

	if (p->p_evpdp && GUESTP(p)->gp_prof != pf)
		gc_free(p);
	if (pf) {
		if (!p->p_evpdp)
			(void)gc_alloc(p, pf);
		if (pf->gpf_exec)
			(*pf->gpf_exec)(GUESTP(p));
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
	cg->gp_flags = pg->gp_flags;
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
	{ 0, 0 }
};

MOD_HOOK_WRAPPER(guestcore, 0, guestcore_unload, "guest process core");
