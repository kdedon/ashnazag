/*
 * guest_env.h -- environment sessions.
 *
 * An environment module fills a struct guest_env (its name, device,
 * hooks and the dispositions in ge_prof.gpf_disp); guestcore builds
 * the rest of the profile and runs the sessions: the device node, a
 * session's claim, attach and exit, the clock tick, virtual interrupts
 * carried by ge_sig, stop, one host-call gate and constant pages.
 *
 *	tick, event --> gs_pend --> kick --> ge_sig, or wakeup if stopped
 *	carrier: while ge_level > vIPL: ge_ack --> frame through the vector
 *	host call: ge_hostvec at a stub --> ge_hostcall(n) --> pc += ge_hostlen
 *	constant page: reads are memory; a write --> ge_access
 *
 * Needs guest.h and guest_rpage.h.  K&R C.
 */

#ifndef _GUEST_ENV_H
#define _GUEST_ENV_H

#include "guest_mem.h"

/* gs_state */
#define	GS_FREE		0
#define	GS_RUN		1
#define	GS_CLAIMED	2

/* ge_flags */
#define	GEF_SIGTAIL	0x01	/* an interrupt only where a signal return keeps it */
#define	GEF_ONSTACK	0x02	/* the carrier waits while a host signal handler runs */
#define	GEF_ONE		0x04	/* one interrupt per carrier */
#define	GEF_HOSTHOLD	0x08	/* the carrier waits while the host's own code runs (GUEST_INHOST) */

#define	GH_IPL		0x80000000UL	/* gs_halt: until a level above the IPL */

/* a page whose reads are constants: cp_dlen bytes of cp_data, then cp_fill */
struct guest_cpage {
	unsigned long	cp_va;		/* page aligned */
	unsigned long	cp_len;		/* whole pages */
	unsigned char	*cp_data;
	unsigned long	cp_dlen;
	unsigned char	cp_fill;
	int		cp_id;		/* the environment's tag */
	unsigned long	cp_off;		/* guestcore's: offset in the session's object */
};

struct guest_sess {
	struct guest_env	*gs_env;
	int			gs_state;
	struct proc		*gs_proc;
	struct guest_proc	*gs_gp;
	int			gs_tid;		/* tick timeout, 0 none */
	int			gs_sleeping;
	unsigned long		gs_pend;	/* bit n: level n asks */
	unsigned long		gs_halt;	/* stopped until one of these levels, or GH_IPL; 0 running */
	unsigned long		gs_ints;	/* interrupts taken */
	struct guest_cpage	*gs_cpage;
	int			gs_ncpage;
	struct guest_ram	*gs_cram;	/* the constant pages, then the sink */
	unsigned long		gs_sinkva;	/* constant page made writable, 0 none */
	unsigned long		gs_sinkoff;
	int			gs_sinktrace;	/* the rerun traced: 1, 3 with the guest's own T1 */
	char			*gs_priv;	/* ge_ssize bytes, zeroed at enter */
};

struct guest_env {
	struct guest_profile	ge_prof;	/* first: gp_prof is the environment */
	char		*ge_name;
	struct modwrapper *ge_wrapper;
	int		ge_major;
	int		ge_entercmd;	/* the ioctl that makes the caller a session */
	int		ge_privsz;	/* per guest process */
	int		ge_ssize;	/* per session */
	unsigned int	ge_gpflags;	/* gp_flags at enter */
	int		ge_nsess;
	int		ge_sig;		/* the carrier */
	unsigned short	ge_holdfmt;	/* bit n: a format n frame finishes before an interrupt */
	unsigned short	ge_flags;	/* GEF_* */
	/* the host-call gate: ge_hostn stubs ge_hoststep apart at ge_hostva, trapping through ge_hostvec */
	unsigned long	ge_hostva;
	int		ge_hostn, ge_hoststep, ge_hostvec, ge_hostlen;
	int	(*ge_enter)();		/* (s, arg): after attach, errno */
	void	(*ge_exit)();		/* (s) */
	int	(*ge_level)();		/* (s): the highest level asking; 0: from gs_pend */
	int	(*ge_sync)();		/* (s): guest context, before the carrier goes in; nonzero holds it */
	int	(*ge_ack)();		/* (s, regs, l): its vector, -1 none; 0: clear the gs_pend bit, autovector */
	void	(*ge_tick)();		/* (s): each clock tick, under spl; 0 no tick */
	int	(*ge_access)();		/* (s, regs, cpage, access): a write; 1 sends it to a copy */
	int	(*ge_hostcall)();	/* (s, regs, n): 0 done, 1 done with the PC set, -1 the vector's disposition */
	int	(*ge_ioctl)();		/* (e, s, cmd, arg, mode, rvp): the other commands */
	void	(*ge_note)();		/* (s, what, a, pc): guestcore's notices; 0 printf */
	struct guest_ramdev	ge_ram;		/* the device's mmap, freed at the last close */
	/* guestcore's */
	struct guest_disp	ge_hostdisp, ge_faultdisp, ge_tracedisp;
	struct guest_sess	*ge_sess;
	int			ge_nopen;
};

#define	GENV(gp)	((struct guest_env *)(gp)->gp_prof)

extern int guest_env_add();		/* (e): at load */
extern int guest_env_del();		/* (e): at unload; EBUSY */
extern struct guest_sess *guest_env_sess();	/* (gp): its session, 0 none */
extern int guest_env_level();		/* (s) */
extern void guest_env_kick();		/* (s): wake or signal for a level above the IPL */
extern void guest_env_post();		/* (s, l): level l asks */
extern void guest_env_intr();		/* (gp, regs): the carrier loop */
extern void guest_env_after();		/* (s): stop's wait, then the trap tail */
extern void guest_env_note();		/* (s, what, a, pc) */
extern int guest_env_open();		/* (e, devp, flags, otyp, cr) */
extern int guest_env_close();		/* (e, dev, flags, otyp, cr) */
extern int guest_env_ioctl();		/* (e, dev, cmd, arg, mode, cr, rvp) */
extern int guest_env_mmap();		/* (e, dev, off, prot) */
extern int guest_env_segmap();		/* (e, dev, off, as, addrp, len, prot, maxprot, flags, cr) */
extern int guest_cpage_map();		/* (s, cp, n): from ge_enter */
extern void guest_cpage_unsink();	/* (s, regs or 0) */
extern int guest_cpage_set();		/* (s, va, bytes, n) */
extern void guest_cpage_free();		/* (s) */
extern int guest_cpage_fault();		/* disposition for vector 2 */
extern int guest_cpage_trace();		/* for vector 9 */

#endif	/* _GUEST_ENV_H */
