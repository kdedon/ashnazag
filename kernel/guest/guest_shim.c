/*
 * guest_shim.c -- static part of guest-process support: the events
 * stubs, the sendsig, valid_usr_range and fsig wrappers, the fatal
 * user-fault notice filter, the hook table and the C entry of the
 * vector gates.
 *
 * A shim calls its hook only for a guest process (p_evpdp set), which
 * holds the module that owns the hook, or for the process whose exec
 * guest_loading names, which runs inside that module's exec call.
 * Everything else takes the stock path.
 *
 * K&R C.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/immu.h"
#include "sys/signal.h"
#include "sys/fs/s5dir.h"
#include "sys/psw.h"
#include "sys/pcb.h"
#include "sys/user.h"
#include "sys/proc.h"
#include "sys/syscall.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "sys/moddefs.h"
#include "guest.h"

extern struct proc *curproc;

struct proc		*guest_loading;
struct guest_profile	*guest_loadprof;

int	(*guest_trap_hook)();
void	(*guest_exec_hook)();
int	(*guest_fork_hook)();
void	(*guest_exit_hook)();
int	(*guest_sendsig_hook)();
int	(*guest_vur_hook)();
int	(*guest_fsig_hook)();
int	(*guest_unote_hook)();
long	guest_nlinea, guest_lineapc;

struct hooksw hooksw[] = {
	{ "guest_trap",		(char **)&guest_trap_hook,	0, 0 },
	{ "guest_exec",		(char **)&guest_exec_hook,	0, 0 },
	{ "guest_fork",		(char **)&guest_fork_hook,	0, 0 },
	{ "guest_exit",		(char **)&guest_exit_hook,	0, 0 },
	{ "guest_sendsig",	(char **)&guest_sendsig_hook,	0, 0 },
	{ "guest_vur",		(char **)&guest_vur_hook,	0, 0 },
	{ "guest_fsig",		(char **)&guest_fsig_hook,	0, 0 },
	{ "guest_unote",	(char **)&guest_unote_hook,	0, 0 },
	{ 0, 0, 0, 0 }
};

/* the vector gates read p_evpdp, p_sig, p_hold and gp_flags, gp_vsr, gp_vvbr at these offsets */
extern char guest_evpdp_at_c8[(int)&((struct proc *)0)->p_evpdp == 0xc8 ? 1 : -1];
extern char guest_sig_at_9c[(int)&((struct proc *)0)->p_sig == 0x9c &&
    (int)&((struct proc *)0)->p_hold == 0xa4 ? 1 : -1];
extern char guest_vcpu_at[(int)&((struct guest_proc *)0)->gp_flags == 12 &&
    (int)&((struct guest_proc *)0)->gp_vsr == 20 &&
    (int)&((struct guest_proc *)0)->gp_vvbr == 28 ? 1 : -1];

/*
 * procdup: ev_fork runs only when this says the parent is a guest.
 * hrtsys asks too; it keeps its ENOPKG.
 */
int
ev_config()
{
	return curproc && curproc->p_evpdp != 0 && u.u_syscall != SYS_hrtsys;
}

/* in the parent, before the child can run; nonzero fails the fork */
int
ev_fork(pp, cp)
	struct proc *pp, *cp;
{
	cp->p_evpdp = 0;
	if (pp->p_evpdp && guest_fork_hook)
		return (*guest_fork_hook)(pp, cp);
	return 0;
}

/* remove_proc, at the point of no return of every exec */
void
ev_exec(p)
	struct proc *p;
{
	if ((p->p_evpdp || guest_loading == p) && guest_exec_hook)
		(*guest_exec_hook)(p);
}

/* exit, with the address space still present */
void
ev_exit(p, stat)
	struct proc *p;
	int stat;
{
	if (p->p_evpdp && guest_exit_hook)
		(*guest_exit_hook)(p, stat);
}

int
sendsig(sig, sip, hdlr)
	int sig;
	char *sip;
	int (*hdlr)();
{
	if (curproc->p_evpdp && guest_sendsig_hook)
		return (*guest_sendsig_hook)(sig, sip, hdlr);
	return __amix_sendsig(sig, sip, hdlr);
}

int
valid_usr_range(a, len)
	caddr_t a;
	u_int len;
{
	struct proc *p = curproc;

	if (p && p->p_evpdp && guest_vur_hook)
		return (*guest_vur_hook)(a, len);
	return __amix_valid_usr_range(a, len);
}

/* issig: the next signal to take; a guest's profile may hold some back */
int
fsig(p)
	struct proc *p;
{
	if (p->p_evpdp && guest_fsig_hook)
		return (*guest_fsig_hook)(p);
	return __amix_fsig(p);
}

/*
 * The "User BUS ERROR" notice of a fatal user fault.  A guest whose
 * own vector takes the fault does not die of it: no notice.
 */
#pragma weak __amix_unt_latch
extern void __amix_unt_latch(), cmn_err();

void
unt_latch(l, f, a, b, c, d, e)
	int l;
	char *f;
	long a, b, c, d, e;
{
	struct proc *p = curproc;

	if (p && p->p_evpdp && guest_unote_hook && (*guest_unote_hook)(p) == 0)
		return;
	if (__amix_unt_latch)
		__amix_unt_latch(l, f, a, b, c, d, e);
	else
		cmn_err(l, f, a, b, c, d, e);
}

/* from the vector gates, guest processes only: 0 handled, else decline */
int
guest_trap(r)
	char *r;
{
	if (guest_trap_hook)
		return (*guest_trap_hook)(r);
	return 1;
}
