/*
 * auxsig.c -- A/UX signals: numbers in kill and ssig, and the SVR3
 * signal frame.
 *
 * SVR3 frame (ssig handlers; the handler is libc's _sigcode), from the
 * new user sp up:
 *
 *	+0  signal (A/UX)    +4  mask at delivery
 *	+8  flag word (0)    +10 SR    +12 PC
 *
 * With flag 0, _sigcode returns by itself: pops 10 bytes, then rtr.
 * A nonzero flag (private state above the frame) returns through
 * sysm68k(2); this kernel never builds one, but accepts the call.
 *
 * BSD frame (sigvec handlers, COMPAT_BSDSIGNALS), 0x40 bytes from F:
 *
 *	+0x00 return address F+0x24   +0x04 signal   +0x08 code
 *	+0x0c &sigcontext (F+0x2c)    +0x10 Mac levels, flag word (0)
 *	+0x14 d0, d1, a0, a1          +0x24 move.l #150,d0; trap #15
 *	+0x2c sigcontext { onstack, mask, sp, pc, ps }
 *
 * The handler's rts runs the stub; sigcleanup (trap #15, 150) reads
 * the frame at usp - 4 and resumes the interrupted context.
 *
 * K&R C.
 */

#include "auxcore.h"
#include "sys/sysm68k.h"

extern int aux_amix();
extern void dlm_cacheflush();
extern int pause();
extern k_sigset_t cantmask;

#define	ABIT(s)		(1L << ((s) - 1))
#define	A_KILLSTOP	(ABIT(9) | ABIT(23))
#define	SV_ONSTACK	0x01
#define	SV_INTERRUPT	0x02
#define	SV_NOCLDSTOP	0x10
#define	BSDFR		0x40

static int
bsdframe(ap, asig, hdlr)
	struct aux_proc *ap;
	int asig;
	int (*hdlr)();
{
	char *r = (char *)u.u_ar0, b[BSDFR];
	long usp = GR_USP(r), f;
	int onst = ap->ap_ss_onstack;

	if ((ap->ap_sv_onstack & ABIT(asig)) && !onst && ap->ap_ss_sp) {
		f = ap->ap_ss_sp - BSDFR;
		ap->ap_ss_onstack = 1;
		ap->ap_ss_isp = usp;
	} else
		f = usp - BSDFR;
	bzero(b, sizeof b);
	P32(b, f + 0x24);
	P32(b + 0x04, asig);
	P32(b + 0x0c, f + 0x2c);
	P32(b + 0x14, GR_D(r, 0));
	P32(b + 0x18, GR_D(r, 1));
	P32(b + 0x1c, GR_A(r, 0));
	P32(b + 0x20, GR_A(r, 1));
	P32(b + 0x24, 0x203c0000);		/* move.l #150,d0 */
	P32(b + 0x28, 0x00964e4f);		/* trap #15 */
	P32(b + 0x2c, onst);
	P32(b + 0x30, aux_mask_out((u_long)u.u_sigoldmask));
	P32(b + 0x34, usp);
	P32(b + 0x38, GR_PC(r));
	P32(b + 0x3c, GR_SR(r));
	if (copyout(b, (caddr_t)f, sizeof b)) {
		ap->ap_ss_onstack = onst;
		return 0;
	}
	dlm_cacheflush();			/* the stub is code */
	GR_USP(r) = f;
	GR_PC(r) = (long)hdlr;
	GR_SR(r) &= ~0xc000;
	u.u_sigflag |= USTKCLEAR;
	return 1;
}

/* trap #15, d0 = 150: back from a BSD handler */
void
aux_sigcleanup(ap, r)
	struct aux_proc *ap;
	char *r;
{
	char b[0x24], c[20];
	long f = GR_USP(r) - 4;
	struct proc *p = u.u_procp;

	if (copyin((caddr_t)f, b, sizeof b) ||
	    copyin((caddr_t)G32(b + 0x0c), c, sizeof c)) {
		psignal(p, SIGSEGV);
		return;
	}
	ap->ap_ss_onstack = G32(c) & 1;
	p->p_hold = aux_mask_in(G32(c + 4) & ~A_KILLSTOP) & ~cantmask;
	GR_D(r, 0) = G32(b + 0x14);
	GR_D(r, 1) = G32(b + 0x18);
	GR_A(r, 0) = G32(b + 0x1c);
	GR_A(r, 1) = G32(b + 0x20);
	GR_USP(r) = G32(c + 8);
	GR_PC(r) = G32(c + 12);
	GR_SR(r) = G32(c + 16) & 0xc0ff;		/* user mode: CCR and trace */
}

int
aux_sendsig(gp, sig, sip, hdlr)
	struct guest_proc *gp;
	int sig;
	char *sip;
	int (*hdlr)();
{
	char *r = (char *)u.u_ar0, b[16];
	long usp = GR_USP(r) - 16;

	if (AUXP(gp)->ap_compat & COMPAT_BSDSIGNALS)
		return bsdframe(AUXP(gp), aux_sig_out(sig), hdlr);

	P32(b, aux_sig_out(sig));
	P32(b + 4, aux_mask_out((u_long)u.u_sigoldmask));
	P16(b + 8, 0);
	P16(b + 10, GR_SR(r));
	P32(b + 12, GR_PC(r));
	if (copyout(b, (caddr_t)usp, sizeof b))
		return 0;
	GR_USP(r) = usp;
	GR_PC(r) = (long)hdlr;
	GR_SR(r) &= ~0xc000;
	u.u_sigflag |= USTKCLEAR;
	return 1;
}

/* sysm68k(cmd, ...): 2 is the SVR3 signal return */
int
aux_sysm68k(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	long usp, fs;

	if (a[0] != 2)
		return EINVAL;
	usp = GR_USP(r);
	fs = fuword((caddr_t)(usp + 0xc));
	GR_PC(r) = fuword((caddr_t)(usp + 0x10));
	GR_SR(r) = (GR_SR(r) & ~0xff) | (fs & 0xff);
	GR_USP(r) = (fs >> 16) & 0xffff ? fuword((caddr_t)(usp + 0x14)) : usp + 0x14;
	rv->r_val1 = a[1];
	return 0;
}

/* ssig(sig, handler): SVR3 signal(), only without BSD signals */
int
aux_ssig(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int s = aux_sig_in((int)a[0]), e;

	if ((ap->ap_compat & COMPAT_BSDSIGNALS) || s <= 0 || s == SIGKILL)
		return EINVAL;
	a[0] = s;
	if (a[1] == 3)
		a[1] = (long)SIG_HOLD;
	if ((e = aux_amix(48, a, rv)) == 0 && rv->r_val1 == (int)SIG_HOLD)
		rv->r_val1 = 3;
	return e;
}

int
aux_kill(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int s = aux_sig_in((int)a[1]);

	if (s < 0)
		return EINVAL;
	a[1] = s;
	return aux_amix(37, a, rv);
}

/* calls a BSD system restarts after a handler (unless SV_INTERRUPT) */
int
aux_restartable(n)
	int n;
{
	switch (n) {
	case 3: case 4: case 7: case 54: case 106: case 107: case 151:
	case 79: case 80: case 81: case 83: case 84: case 85:
		return 1;
	}
	return 0;
}

/* sigvec(sig, nsv, osv): struct sigvec { handler, mask, flags } */
int
aux_sigvec(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int as = a[0], s = aux_sig_in(as), fl = 0;
	long sv[3];

	if (!(ap->ap_compat & COMPAT_BSDSIGNALS) || s <= 0)
		return EINVAL;
	if (a[1] && (as == 9 || as == 23))
		return EINVAL;
	if (a[1] && copyin((caddr_t)a[1], (caddr_t)sv, sizeof sv))
		return EFAULT;
	if (a[2]) {
		long o[3];

		o[0] = (long)u.u_signal[s - 1];
		o[1] = aux_mask_out((u_long)u.u_sigmask[s - 1]);
		o[2] = (ap->ap_sv_onstack & ABIT(as) ? SV_ONSTACK : 0) |
		    (ap->ap_sv_intr & ABIT(as) ? SV_INTERRUPT : 0);
		if (copyout((caddr_t)o, (caddr_t)a[2], sizeof o))
			return EFAULT;
	}
	if (a[1] == 0)
		return 0;
	if (sv[0] == 3) {			/* SIG_HOLD: block, keep the action */
		u.u_procp->p_hold |= aux_mask_in(ABIT(as)) & ~cantmask;
		return 0;
	}
	ap->ap_sv_onstack &= ~ABIT(as);
	ap->ap_sv_intr &= ~ABIT(as);
	if (sv[2] & SV_ONSTACK)
		ap->ap_sv_onstack |= ABIT(as);
	if (sv[2] & SV_INTERRUPT)
		ap->ap_sv_intr |= ABIT(as);
	else
		fl |= SA_RESTART;
	if (sv[2] & SV_NOCLDSTOP)
		fl |= SA_NOCLDSTOP;
	setsigact(s, (void (*)())sv[0],
	    (k_sigset_t)(aux_mask_in(sv[1] & ~A_KILLSTOP) & ~cantmask), fl);
	return 0;
}

int
aux_sigblock(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct proc *p = u.u_procp;

	if (!(ap->ap_compat & COMPAT_BSDSIGNALS))
		return EINVAL;
	rv->r_val1 = aux_mask_out((u_long)p->p_hold);
	p->p_hold |= aux_mask_in(a[0]) & ~cantmask;
	return 0;
}

int
aux_sigsetmask(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct proc *p = u.u_procp;

	if (!(ap->ap_compat & COMPAT_BSDSIGNALS))
		return EINVAL;
	rv->r_val1 = aux_mask_out((u_long)p->p_hold);
	p->p_hold = aux_mask_in(a[0]) & ~cantmask;
	return 0;
}

/* as sigsuspend: the old mask comes back with the handler's frame */
int
aux_sigpause(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct proc *p = u.u_procp;

	if (!(ap->ap_compat & COMPAT_BSDSIGNALS))
		return EINVAL;
	u.u_sigoldmask = p->p_hold;
	p->p_hold = aux_mask_in(a[0]) & ~cantmask;
	u.u_sigflag |= SOMASK;
	return pause();
}

/* sigstack(nss, oss): struct sigstack { sp, onstack } */
int
aux_sigstack(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	long ss[2];

	if (!(ap->ap_compat & COMPAT_BSDSIGNALS))
		return EINVAL;
	if (a[1]) {
		ss[0] = ap->ap_ss_sp;
		ss[1] = ap->ap_ss_onstack;
		if (copyout((caddr_t)ss, (caddr_t)a[1], sizeof ss))
			return EFAULT;
	}
	if (a[0]) {
		if (copyin((caddr_t)a[0], (caddr_t)ss, sizeof ss))
			return EFAULT;
		ap->ap_ss_sp = ss[0];
		ap->ap_ss_onstack = ss[1] & 1;
	}
	return 0;
}
