/*
 * auxcore.h -- the A/UX personality: per-process state, the call
 * table entry, conversions.
 */

#ifndef _AUXCORE_H
#define _AUXCORE_H

#include "kinc.h"
#include "sys/vnode.h"
#include "sys/file.h"
#include "sys/uio.h"
#include "sys/time.h"

/* A/UX compat word (setcompat) */
#define	COMPAT_BSDGROUPS	0x0001
#define	COMPAT_BSDNBIO		0x0002
#define	COMPAT_BSDSIGNALS	0x0004
#define	COMPAT_BSDTTY		0x0008
#define	COMPAT_SYSCALLS		0x0010
#define	COMPAT_CLRPGROUP	0x0020
#define	COMPAT_EXEC		0x0100
#define	COMPAT_DEFAULT		0x0403

struct aux_proc {
	u_int	ap_compat;
	u_int	ap_flags;
	u_long	ap_sv_onstack;	/* sigvec SV_ONSTACK, A/UX signal bits */
	u_long	ap_sv_intr;	/* sigvec SV_INTERRUPT */
	long	ap_ss_sp;	/* sigstack top, 0 = none */
	int	ap_ss_onstack;
	long	ap_ss_isp;	/* sp the signal stack was entered from */
	int	ap_itid;	/* ITIMER_REAL callout, 0 = disarmed */
	long	ap_itexp;	/* its expiry, lbolt */
	long	ap_itint;	/* its interval, ticks */
	u_int	ap_mac;		/* APM_* */
	int	ap_alid;	/* alarm callout, 0 = none */
	long	ap_alexp;	/* its expiry, lbolt */
};
#define	APM_TASK	0x01	/* a task of the Mac environment's layer */
#define	APM_UIP		0x02	/* holds the ui page */
#define	AUXP(gp)	((struct aux_proc *)GUEST_PRIV(gp))

/* a call: class N has ae_amix and no ae_fn */
struct auxent {
	short	ae_num;		/* A/UX number */
	char	ae_narg;	/* trap #0 argument longs */
	char	ae_flags;
	short	ae_amix;	/* AMIX sysent number, class N */
	int	(*ae_fn)();	/* (ap, args, rvp, regs): errno (AMIX) */
};
#define	AE_SETJMP	0x01
#define	AE_NOSYS	0x02	/* A/UX nosys: SIGSYS, no error */
#define	AE_TODO		0x04	/* not implemented yet: EINVAL */

#define	AUX_NCALL	256

#define	AUX_OGLOBAL	0x80000000	/* open: a Mac-side (global) file */
#define	AUX_GFD		128		/* the first global fd */
#define	AUX_GNOFILE	64		/* global fds */

/* A/UX errno values the personality returns directly */
#define	AUX_EWOULDBLOCK	55
#define	AUXE_WOULDBLOCK	254	/* internal: EWOULDBLOCK whatever the compat */
#define	AUXE_AGAIN	253	/* internal: EAGAIN whatever the compat */

extern struct auxent auxcalls[];
extern struct guest_profile aux_profile;
extern struct modwrapper auxcore_wrapper;
extern int aux_trace;
extern void (*aux_macdetach)();	/* (gp): the layer's, while loaded */
extern int (*aux_slotmgr)();	/* (selector, SpBlock, &result): errno */
extern void (*aux_uitick)();	/* (gp): a Mac task takes its tick */
#define	AUX_TBUF	8192
extern void aux_tlog();			/* (pid, text, value): trace ring */
extern void aux_thex();			/* (tag, args, n, uaddr, nbytes, value) */
extern long mac_socktrace;

/* auxconv.c */
extern int aux_errno_out();
extern int aux_sig_in(), aux_sig_out();
extern u_long aux_mask_in(), aux_mask_out();
extern int aux_wstat_out();
extern int aux_oflags_in(), aux_oflags_out();

/* auxmisc.c */
extern caddr_t aux_gap();
extern void aux_itstop();
extern void aux_alstop();
extern int aux_fcntl_lk(), aux_tiocpkt(), aux_waitcom();

/* auxfid.c */
extern void aux_fidfree();

/* auxsig.c */
extern int aux_sendsig();
extern int aux_sysm68k();
extern u_long aux_fmgrflag;
extern int aux_ssig(), aux_kill();
extern void aux_sigcleanup();
extern int aux_restartable();

#endif	/* _AUXCORE_H */
