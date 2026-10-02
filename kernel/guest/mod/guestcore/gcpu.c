/*
 * gcpu.c -- virtual supervisor state of a guest process: A-line
 * reflection (the path behind the gate's fast one), emulation of
 * privileged instructions against the virtual SR, and signals held by
 * the virtual interrupt level.
 *
 * The real SR of a guest keeps user mode, T1 and the condition codes;
 * S, M and the IPL live in gp_vsr.  With GPF_SPIN, S stays set and the
 * guest runs on one stack (the Mac profile); otherwise a change of S
 * swaps A7 with gp_vusp, the inactive stack pointer.
 *
 * K&R C.
 */

#include "kinc.h"
#include "sys/siginfo.h"
#include "sys/sysm68k.h"

extern void dlm_cacheflush();
extern int preempt(), issig();
extern void psig();
extern int runrun;

#define	SR_REAL		0x80ff		/* T1 and CCR */
#define	SR_VIRT		0x3700		/* S, M, IPL */
#define	SR_IPL		0x0700
#define	SR_S		0x2000
/*
 * What the virtual IPL never holds: SIGQUIT, SIGKILL, SIGTERM, and the
 * exception signals.  An exception the IPL held would leave the PC on
 * the faulting instruction and trap again at once.
 */
#define	VIPL_PASS	(sigmask(SIGQUIT) | sigmask(SIGKILL) | sigmask(SIGTERM) | \
			 sigmask(SIGILL) | sigmask(SIGTRAP) | sigmask(SIGEMT) | \
			 sigmask(SIGFPE) | sigmask(SIGBUS) | sigmask(SIGSEGV))

int	guest_npriv;			/* emulated instructions */
int	guest_nprivbad;			/* refused ones */
long	guest_npk[12];			/* emulated, by privkind() */
long	guest_privop;			/* the last refused opcode */
long	guest_nfault7, guest_f7pc, guest_f7ea;	/* faults sent to vector 2 */
long	guest_nunote;			/* their notices left out */

static int
splhi_()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}

static void
splx_(s)
	int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

/* the SR the guest sees */
static int
vsr_get(gp, r)
	struct guest_proc *gp;
	char *r;
{
	return (gp->gp_vsr & SR_VIRT) | (GR_SR(r) & SR_REAL);
}

static void
vsr_set(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	long t;

	GR_SR(r) = (GR_SR(r) & ~SR_REAL) | (v & SR_REAL);
	if (!(gp->gp_flags & GPF_SPIN) && ((v ^ gp->gp_vsr) & SR_S)) {
		t = GR_USP(r);		/* the other stack becomes active */
		GR_USP(r) = gp->gp_vusp;
		gp->gp_vusp = t;
	}
	gp->gp_vsr = (v & SR_VIRT) | (gp->gp_flags & GPF_SPIN ? SR_S : 0);
	if ((v & SR_IPL) == 0)
		gp->gp_flags &= ~GPF_VPEND;	/* held signals go out on the way back */
}

/* the stock trap tail: reschedule, then deliverable signals */
static void
trapret()
{
	struct proc *p = u.u_procp;
	struct guest_proc *gp = GUESTP(p);

	if (gp && gp->gp_prof->gpf_intr)
		(*gp->gp_prof->gpf_intr)(gp, (char *)u.u_ar0);
	if (runrun)
		preempt();
	if (p->p_cursig || p->p_sig || (p->p_flag & SPRSTOP))
		if (issig(0))
			psig();
}

/* for profile modules */
int
guest_getsr(gp, r)
	struct guest_proc *gp;
	char *r;
{
	return vsr_get(gp, r);
}

void
guest_setsr(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	vsr_set(gp, r, v);
}

void
guest_trapret()
{
	trapret();
}

/*
 * Exception entry through the guest's vector table: the frame (SR, PC,
 * then fv and n - 2 bytes from x) on the supervisor stack, S set, T
 * cleared, the IPL raised to ipl unless it is negative.  -1: the stack
 * or the vector is not readable.
 */
int
guest_reflect(gp, r, pc, fv, x, n, ipl)
	struct guest_proc *gp;
	char *r, *x;
	long pc;
	int fv, n, ipl;
{
	char f[64];
	long h, sp;
	int sr = vsr_get(gp, r), nsr;

	if (n < 2 || n > sizeof f - 6)
		return -1;
	P16(f, sr);
	P32(f + 2, pc);
	P16(f + 6, fv);
	if (n > 2)
		bcopy(x, f + 8, n - 2);
	nsr = (sr & ~0xc000) | SR_S;
	if (ipl >= 0)
		nsr = (nsr & ~SR_IPL) | ipl << 8;
	sp = (sr & SR_S) || (gp->gp_flags & GPF_SPIN) ? GR_USP(r) : gp->gp_vusp;
	sp -= 6 + n;
	if (copyin((caddr_t)(gp->gp_vvbr + (fv & 0xfff)), (caddr_t)&h, 4) ||
	    copyout(f, (caddr_t)sp, 6 + n))
		return -1;
	vsr_set(gp, r, nsr);
	GR_USP(r) = sp;
	GR_PC(r) = h;
	return 0;
}

/*
 * Vector 10: the 8-byte format-0 frame at the user sp, then the guest's
 * handler at vVBR + $28.  The gate's fast path does the same.
 */
int
guest_aline(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	char f[8];
	long h, usp;

	if (!(gp->gp_flags & GPF_ALINE))
		return 1;
	guest_nlinea++;
	guest_lineapc = GR_PC(r);
	usp = GR_USP(r) - 8;
	P16(f, GR_SR(r) | gp->gp_vsr);
	P32(f + 2, GR_PC(r));
	P16(f + 6, 0x0028);
	if (copyin((caddr_t)(gp->gp_vvbr + 0x28), (caddr_t)&h, 4) ||
	    copyout(f, (caddr_t)usp, 8)) {
		psignal(curproc, SIGSEGV);
		trapret();
		return 0;
	}
	GR_USP(r) = usp;
	GR_PC(r) = h;
	GR_SR(r) &= 0x00ff;
	gp->gp_vsr &= SR_S | SR_IPL;
	trapret();
	return 0;
}

/* ---- instruction decoding ---- */

struct pi {
	char	*r;
	long	pc;		/* of the instruction */
	int	len;		/* bytes consumed */
};

static int
iword(pi, w)
	struct pi *pi;
	u_short *w;
{
	char b[2];

	if (copyin((caddr_t)(pi->pc + pi->len), b, 2))
		return -1;
	pi->len += 2;
	*w = G16(b);
	return 0;
}

static long *
areg(pi, n)
	struct pi *pi;
	int n;
{
	return n == 7 ? &GR_USP(pi->r) : &GR_A(pi->r, n);
}

/* index register of a brief extension word */
static long
xreg(pi, ext)
	struct pi *pi;
	int ext;
{
	long x = ext & 0x8000 ? *areg(pi, (ext >> 12) & 7) : GR_D(pi->r, (ext >> 12) & 7);

	if (!(ext & 0x800))
		x = (short)x;
	return x << ((ext >> 9) & 3);
}

/*
 * The address of a memory operand of size sz (1, 2, 4); (An)+ and -(An)
 * update An.  Immediate: *immp gets the value, the result is 0.
 * -1: not a form handled here (register direct, full extension words).
 */
static int
eaddr(pi, mode, reg, sz, ap, immp)
	struct pi *pi;
	int mode, reg, sz;
	long *ap, *immp;
{
	u_short w, w2;
	long base;
	int inc = reg == 7 && sz == 1 ? 2 : sz;

	*immp = 0;
	switch (mode) {
	case 2:
		*ap = *areg(pi, reg);
		return 1;
	case 3:
		*ap = *areg(pi, reg);
		*areg(pi, reg) += inc;
		return 1;
	case 4:
		*areg(pi, reg) -= inc;
		*ap = *areg(pi, reg);
		return 1;
	case 5:
		if (iword(pi, &w))
			return -1;
		*ap = *areg(pi, reg) + (short)w;
		return 1;
	case 6:
		if (iword(pi, &w) || (w & 0x100))
			return -1;
		*ap = *areg(pi, reg) + (char)(w & 0xff) + xreg(pi, (int)w);
		return 1;
	case 7:
		switch (reg) {
		case 0:
			if (iword(pi, &w))
				return -1;
			*ap = (short)w;
			return 1;
		case 1:
			if (iword(pi, &w) || iword(pi, &w2))
				return -1;
			*ap = (long)w << 16 | w2;
			return 1;
		case 2:
			base = pi->pc + pi->len;
			if (iword(pi, &w))
				return -1;
			*ap = base + (short)w;
			return 1;
		case 3:
			base = pi->pc + pi->len;
			if (iword(pi, &w) || (w & 0x100))
				return -1;
			*ap = base + (char)(w & 0xff) + xreg(pi, (int)w);
			return 1;
		case 4:
			if (iword(pi, &w))
				return -1;
			if (sz == 4) {
				if (iword(pi, &w2))
					return -1;
				*immp = (long)w << 16 | w2;
			} else
				*immp = sz == 1 ? (w & 0xff) : w;
			return 0;
		}
	}
	return -1;
}

/* read (wr 0) or write an operand of size sz; register forms included */
static int
operand(pi, mode, reg, sz, vp, wr, src)
	struct pi *pi;
	int mode, reg, sz, wr;
	long *vp;
	int src;		/* immediate and PC-relative allowed */
{
	char b[4];
	long a, imm, *rp, m = sz == 4 ? -1L : sz == 2 ? 0xffffL : 0xffL;
	int k;

	if (mode == 0 || mode == 1) {
		if (mode == 1 && sz == 1)
			return -1;
		rp = mode == 0 ? &GR_D(pi->r, reg) : areg(pi, reg);
		if (wr)
			*rp = mode == 1 ? (sz == 2 ? (short)*vp : *vp) : (*rp & ~m) | (*vp & m);
		else
			*vp = *rp & m;
		return 0;
	}
	if ((k = eaddr(pi, mode, reg, sz, &a, &imm)) < 0)
		return -1;
	if (k == 0 || (mode == 7 && (reg == 2 || reg == 3))) {
		if (wr || !src)
			return -1;
		if (k == 0) {
			*vp = imm;
			return 0;
		}
	}
	if (wr) {
		if (sz == 4)
			P32(b, *vp);
		else if (sz == 2)
			P16(b, *vp);
		else
			P8(b, *vp);
		return copyout(b, (caddr_t)a, sz) ? -2 : 0;
	}
	if (copyin((caddr_t)a, b, sz))
		return -2;
	*vp = sz == 4 ? (long)G32(b) : sz == 2 ? (long)G16(b) : (long)G8(b);
	return 0;
}

/* movec control register: 0 known, -1 not */
static int
creg(gp, pi, c, vp, wr)
	struct guest_proc *gp;
	struct pi *pi;
	int c, wr;
	long *vp;
{
	unsigned long *cp;
	unsigned long zero = 0;

	switch (c) {
	case 0x000: cp = &gp->gp_vsfc; break;
	case 0x001: cp = &gp->gp_vdfc; break;
	case 0x002: cp = &gp->gp_vcacr; break;
	case 0x800: cp = &gp->gp_vusp; break;
	case 0x801: cp = &gp->gp_vvbr; break;
	case 0x803: case 0x804:		/* MSP, ISP: the one stack */
		cp = (unsigned long *)&GR_USP(pi->r);
		break;
	case 0x003: case 0x004: case 0x005: case 0x006: case 0x007:
	case 0x802: case 0x805: case 0x806: case 0x807:
		cp = &zero;		/* MMU and CAAR: recorded nowhere */
		break;
	default:
		return -1;
	}
	if (!wr) {
		*vp = *cp;
		return 0;
	}
	if (c == 0x801 && (*vp & 1))
		return -1;
	*cp = *vp;
	if (c == 0x002)
		dlm_cacheflush();
	return 0;
}

/* rte: formats 0, 2, 3, 9, $A and $B; the new SR may switch stacks */
static int
vrte(gp, pi)
	struct guest_proc *gp;
	struct pi *pi;
{
	char *r = pi->r, b[8];
	long usp = GR_USP(r);
	int n;

	if (copyin((caddr_t)usp, b, 8))
		return -2;
	switch (G16(b + 6) >> 12) {
	case 0: n = 8; break;
	case 2: case 3: n = 12; break;
	case 9: n = 20; break;
	case 0xa: n = 32; break;		/* 68030 bus faults: rerun */
	case 0xb: n = 92; break;
	default: return -1;
	}
	GR_USP(r) = usp + n;
	GR_PC(r) = G32(b + 2);
	vsr_set(gp, r, (int)G16(b));
	return 1;		/* PC set */
}

extern int fpu_present;
extern void fsave(), frestore();

/* size of a 68040 FPU state frame from its header, 0 if not one */
static int
fplen(b)
	u_char *b;
{
	if (b[0] == 0)
		return 4;
	if (b[0] == 0x41 && (b[1] == 0 || b[1] == 0x30 || b[1] == 0x60))
		return 4 + b[1];
	return 0;
}

/*
 * fsave, frestore through the real unit, which holds the process's own
 * state while it runs.  A fault undoes the fsave.
 */
static int
fpstate(pi, op)
	struct pi *pi;
	int op;
{
	long f[26], a, imm;
	int mode = (op >> 3) & 7, reg = op & 7, n, e;

	if (!fpu_present)
		return -1;
	if (!(op & 0x40)) {
		if (mode < 2 || mode == 3 || (mode == 7 && reg > 1))
			return -1;
		fsave((caddr_t)f);
		if ((n = fplen((u_char *)f)) == 0)
			e = -1;
		else if (eaddr(pi, mode, reg, n, &a, &imm) <= 0)
			e = -1;
		else
			e = copyout((caddr_t)f, (caddr_t)a, n) ? -2 : 0;
		if (e)
			frestore((caddr_t)f);
		else if (n > 4) {
			/* keep the unit live so a switch still saves fp0-fp7 */
			f[0] = 0x41000000;
			frestore((caddr_t)f);
		}
		return e;
	}
	if (mode < 2 || mode == 4 || (mode == 7 && reg > 3))
		return -1;
	if (mode == 3)
		a = *areg(pi, reg);
	else if (eaddr(pi, mode, reg, 4, &a, &imm) <= 0)
		return -1;
	if (copyin((caddr_t)a, (caddr_t)f, 4))
		return -2;
	if ((n = fplen((u_char *)f)) == 0)
		return -1;
	if (n > 4 && copyin((caddr_t)(a + 4), (caddr_t)(f + 1), n - 4))
		return -2;
	if (mode == 3)
		*areg(pi, reg) += n;
	frestore((caddr_t)f);
	return 0;
}

/* counter index: SR ops, rte, SR moves, usp, movec, moves, cache, MMU, fsave, frestore, other */
static int
privkind(op)
	int op;
{
	if (op == 0x007c || op == 0x027c || op == 0x0a7c)
		return 0;
	if (op == 0x4e73)
		return 1;
	if ((op & 0xffc0) == 0x40c0)
		return 2;
	if ((op & 0xffc0) == 0x46c0)
		return 3;
	if ((op & 0xfff0) == 0x4e60)
		return 4;
	if ((op & 0xfffe) == 0x4e7a)
		return 5;
	if ((op & 0xff00) == 0x0e00)
		return 6;
	if ((op & 0xfe00) == 0xf400)
		return 7 + ((op >> 8) & 1);
	if ((op & 0xff80) == 0xf300)
		return 9 + ((op >> 6) & 1);
	return 11;
}

/*
 * One privileged instruction.  0: emulated, PC past it; 1: PC set;
 * -1: not emulated; -2: operand fault.
 */
static int
priv1(gp, pi)
	struct guest_proc *gp;
	struct pi *pi;
{
	char *r = pi->r;
	u_short op, ext;
	long v;
	int sr, mode, reg, e;

	if (iword(pi, &op))
		return -2;
	guest_npk[privkind((int)op)]++;
	mode = (op >> 3) & 7;
	reg = op & 7;
	switch (op) {
	case 0x007c: case 0x027c: case 0x0a7c:		/* ori/andi/eori #,SR */
		if (iword(pi, &ext))
			return -2;
		sr = vsr_get(gp, r);
		sr = op == 0x007c ? sr | ext : op == 0x027c ? sr & ext : sr ^ ext;
		vsr_set(gp, r, sr);
		return 0;
	case 0x4e73:
		return vrte(gp, pi);
	}
	if ((op & 0xffc0) == 0x40c0) {			/* move SR,<ea> */
		v = vsr_get(gp, r);
		return operand(pi, mode, reg, 2, &v, 1, 0);
	}
	if ((op & 0xffc0) == 0x46c0) {			/* move <ea>,SR */
		if ((e = operand(pi, mode, reg, 2, &v, 0, 1)) != 0)
			return e;
		vsr_set(gp, r, (int)v);
		return 0;
	}
	if ((op & 0xfff0) == 0x4e60) {			/* move An,USP / USP,An */
		if (op & 8)
			*areg(pi, reg) = gp->gp_vusp;
		else
			gp->gp_vusp = *areg(pi, reg);
		return 0;
	}
	if ((op & 0xfffe) == 0x4e7a) {			/* movec */
		if (iword(pi, &ext))
			return -2;
		if (op & 1) {
			v = ext & 0x8000 ? *areg(pi, (ext >> 12) & 7) : GR_D(r, (ext >> 12) & 7);
			return creg(gp, pi, ext & 0xfff, &v, 1);
		}
		if (creg(gp, pi, ext & 0xfff, &v, 0))
			return -1;
		if (ext & 0x8000)
			*areg(pi, (ext >> 12) & 7) = v;
		else
			GR_D(r, (ext >> 12) & 7) = v;
		return 0;
	}
	if ((op & 0xff00) == 0x0e00 && (op & 0xc0) != 0xc0) {	/* moves */
		int sz = 1 << ((op >> 6) & 3);
		long *rp;

		if (iword(pi, &ext) || mode < 2)
			return -1;
		rp = ext & 0x8000 ? areg(pi, (ext >> 12) & 7) : &GR_D(r, (ext >> 12) & 7);
		if (ext & 0x800) {
			v = *rp;
			return operand(pi, mode, reg, sz, &v, 1, 0);
		}
		if ((e = operand(pi, mode, reg, sz, &v, 0, 0)) != 0)
			return e;
		if (ext & 0x8000)
			*rp = sz == 1 ? (char)v : sz == 2 ? (short)v : v;
		else
			*rp = (*rp & ~(sz == 4 ? -1L : (1L << 8 * sz) - 1)) | v;
		return 0;
	}
	if ((op & 0xff00) == 0xf400) {			/* cinv, cpush: push all */
		dlm_cacheflush();
		return 0;
	}
	if ((op & 0xff00) == 0xf500)			/* pflush, ptest */
		return 0;
	if ((op & 0xff80) == 0xf300)			/* fsave, frestore */
		return fpstate(pi, (int)op);
	return -1;
}

static int nbadmsg;		/* NOTICE lines printed */

/* vector 8 */
int
guest_priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	struct pi pi;
	char save[64];			/* usp, d0-d7, a0-a6 */
	int e;

	if (!(gp->gp_flags & GPF_PRIV))
		return 1;
	pi.r = r;
	pi.pc = GR_PC(r);
	pi.len = 0;
	bcopy(r, save, sizeof save);
	e = priv1(gp, &pi);
	if (e == 0)
		GR_PC(r) = pi.pc + pi.len;
	if (e >= 0) {
		guest_npriv++;
		trapret();
		return 0;
	}
	bcopy(save, r, sizeof save);
	guest_nprivbad++;
	guest_privop = fuword((caddr_t)pi.pc) >> 16 & 0xffff;
	if (e == -1 && !(gp->gp_flags & GPF_PRIVBAD) && nbadmsg <= 10) {
		gp->gp_flags |= GPF_PRIVBAD;
		if (nbadmsg++ < 10)
			printf("NOTICE: guest pid %d: privileged %x at %x not emulated\n",
			    (int)curproc->p_pid, (int)fuword((caddr_t)pi.pc), (int)pi.pc);
		else
			printf("NOTICE: further unemulated instructions not reported\n");
	}
	if (e == -2) {
		psignal(curproc, SIGSEGV);
		trapret();
		return 0;
	}
	return 1;			/* stock: SIGILL */
}

/*
 * Vector 2 from user mode: note the fault for guest_fault, then the
 * stock handler resolves it or posts the signal.
 */
int
guest_fnote(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	if ((gp->gp_flags & GPF_ALINE) && (GR_FV(r) >> 12) == 7) {
		gp->gp_fpc = GR_PC(r);
		gp->gp_fea = *(long *)(r + 72);
		gp->gp_fssw = *(unsigned short *)(r + 76);
		gp->gp_fpre = gp->gp_proc->p_sig & (sigmask(SIGBUS) | sigmask(SIGSEGV));
		gp->gp_flags = (gp->gp_flags & ~GPF_UNOTE) | GPF_FAULT;
	}
	return 1;
}

/*
 * The signal of an access fault the kernel could not resolve goes to
 * the guest's vector 2 as a 68040 format-7 frame (60 bytes).  Only on
 * the way out of the noted fault: a signal another process sent stays
 * a signal.  0: reflected.
 */
static int
guest_fault(p, gp, n)
	struct proc *p;
	struct guest_proc *gp;
	int n;
{
	char *r = (char *)u.u_ar0, f[60];
	sigqueue_t *sq, *q, **pp;
	long h, usp;
	int s;

	if (!(gp->gp_flags & GPF_FAULT) || gp->gp_fpc != GR_PC(r) || GR_VEC(r) != 2 ||
	    (GR_SR(r) & 0x2000))
		return 1;
	s = splhi_();
	for (sq = p->p_sigqueue; sq && sq->sq_info.si_signo != n; sq = sq->sq_next)
		;
	h = sq && !SI_FROMKERNEL(&sq->sq_info);
	splx_(s);
	if (h)
		return 1;
	usp = GR_USP(r) - sizeof f;
	bzero(f, sizeof f);
	P16(f, GR_SR(r) | gp->gp_vsr);
	P32(f + 2, GR_PC(r));
	P16(f + 6, 0x7008);
	P32(f + 8, gp->gp_fea);
	P16(f + 12, gp->gp_fssw & 0x017f);	/* RW, size, TT, TM */
	P32(f + 20, gp->gp_fea);
	if (copyin((caddr_t)(gp->gp_vvbr + 8), (caddr_t)&h, 4) || h == 0 || (h & 1) ||
	    copyout(f, (caddr_t)usp, sizeof f))
		return 1;
	s = splhi_();
	for (pp = &p->p_sigqueue; *pp && *pp != sq; pp = &(*pp)->sq_next)
		;
	if (sq && *pp)
		*pp = sq->sq_next;
	else
		sq = 0;
	for (q = p->p_sigqueue; q && q->sq_info.si_signo != n; q = q->sq_next)
		;
	if (!q)
		sigdelset(&p->p_sig, n);
	splx_(s);
	if (sq)
		kmem_free((caddr_t)sq, sizeof *sq);
	guest_nfault7++;
	guest_f7pc = GR_PC(r);
	guest_f7ea = gp->gp_fea;
	GR_USP(r) = usp;
	GR_PC(r) = h;
	GR_SR(r) &= 0x00ff;
	gp->gp_vsr &= SR_S | SR_IPL;
	u.u_sigflag |= USTKCLEAR;
	return 0;
}

/*
 * The fatal-fault notice: 0 leaves it out when guest_fault will send
 * this fault to the guest's vector 2.
 */
int
guest_unote(p)
	struct proc *p;
{
	struct guest_proc *gp = GUESTP(p);
	char *r = (char *)u.u_ar0;
	long h;

	if (p != u.u_procp || !(gp->gp_flags & GPF_FAULT) || gp->gp_fpc != GR_PC(r) ||
	    GR_VEC(r) != 2 || (GR_SR(r) & 0x2000) ||
	    copyin((caddr_t)(gp->gp_vvbr + 8), (caddr_t)&h, 4) || h == 0 || (h & 1))
		return 1;
	gp->gp_flags |= GPF_UNOTE;
	guest_nunote++;
	return 0;
}

static int
guest_fsig1(p, gp)
	struct proc *p;
	struct guest_proc *gp;
{
	k_sigset_t h;
	int s, n;

	if (gp->gp_prof->gpf_fsig)
		return (*gp->gp_prof->gpf_fsig)(p, gp);
	if (!(gp->gp_flags & GPF_PRIV) || (gp->gp_vsr & SR_IPL) == 0)
		return __amix_fsig(p);
	s = splhi_();
	if (p->p_sig & ~p->p_hold & ~VIPL_PASS)
		gp->gp_flags |= GPF_VPEND;
	h = p->p_hold;
	p->p_hold |= ~VIPL_PASS;
	n = __amix_fsig(p);
	p->p_hold = h;
	splx_(s);
	return n;
}

/*
 * issig: while the virtual IPL is up, every signal but VIPL_PASS stays
 * pending; with guest vectors, access faults go to vector 2.
 */
int
guest_fsig(p)
	struct proc *p;
{
	struct guest_proc *gp = GUESTP(p);

	/*
	 * The signal the fault's trap posted goes first, while u_ar0 is
	 * still its frame; one already pending before the fault stays a
	 * signal.
	 */
	if ((gp->gp_flags & GPF_FAULT) && p == u.u_procp) {
		k_sigset_t n = p->p_sig & ~gp->gp_fpre;

		if (!sigismember(&n, SIGBUS) || guest_fault(p, gp, SIGBUS))
			if (sigismember(&n, SIGSEGV))
				(void)guest_fault(p, gp, SIGSEGV);
		/* not reflected after all: the notice that was left out */
		if ((gp->gp_flags & GPF_UNOTE) &&
		    (p->p_sig & ~gp->gp_fpre & (sigmask(SIGBUS) | sigmask(SIGSEGV))))
			printf("NOTICE: User BUS ERROR at %x, PC:%x PID:%d CMD:%s\n",
			    (int)gp->gp_fea, (int)gp->gp_fpc, (int)p->p_pid, u.u_comm);
		gp->gp_flags &= ~(GPF_FAULT | GPF_UNOTE);
	}
	return guest_fsig1(p, gp);
}
