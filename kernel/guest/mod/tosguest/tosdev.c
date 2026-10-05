/*
 * tosdev.c -- the TT/ST registers TOS drives directly.
 *
 * The I/O windows ($00F00000-$00FFFFFF and $FF000000-$FFFFFFFF) are not
 * mapped.  An access faults; the instruction is decoded and performed
 * against the register models below, and the guest continues after
 * it.  Registers of absent chips answer with a bus error through the
 * guest's vector 2, which is how TOS probes for them.
 *
 * K&R C.
 */

#include "tos.h"

extern struct seg *as_segat();

/* ---- instruction decoding ---- */

struct ix {
	char	*r;		/* saved frame */
	long	pc;		/* of the instruction */
	int	len;
	long	ba;		/* bus error address */
};

struct opnd {
	int	kind;		/* OK_* */
	int	reg;
	long	a;
};

#define	OK_D	0
#define	OK_A	1
#define	OK_M	2
#define	OK_I	3

#define	CC_C	0x01
#define	CC_V	0x02
#define	CC_Z	0x04
#define	CC_N	0x08
#define	CC_X	0x10

static int ntrace;		/* console lines so far */

static void
trace(what, a, pc)
	char *what;
	long a, pc;
{
	if (tos_trace && ntrace < 60) {
		ntrace++;
		printf("tos: %s %x pc %x\n", what, (int)a, (int)pc);
	}
}

static int
fetch(x, w)
	struct ix *x;
	unsigned long *w;
{
	char b[2];

	if (copyin((caddr_t)(x->pc + x->len), b, 2))
		return -1;
	x->len += 2;
	*w = G16(b);
	return 0;
}

static long *
areg(x, n)
	struct ix *x;
	int n;
{
	return n == 7 ? &GR_USP(x->r) : &GR_A(x->r, n);
}

static long
xreg(x, ext)
	struct ix *x;
	unsigned long ext;
{
	long v = ext & 0x8000 ? *areg(x, (int)(ext >> 12) & 7) : GR_D(x->r, (ext >> 12) & 7);

	if (!(ext & 0x800))
		v = (short)v;
	return v << ((ext >> 9) & 3);
}

/* decode an effective address; (An)+ and -(An) update An.  -1: not handled */
static int
ea(x, mode, reg, sz, o)
	struct ix *x;
	int mode, reg, sz;
	struct opnd *o;
{
	unsigned long w, w2;
	long base;
	int inc = reg == 7 && sz == 1 ? 2 : sz;

	o->reg = reg;
	o->kind = OK_M;
	switch (mode) {
	case 0:
		o->kind = OK_D;
		return 0;
	case 1:
		o->kind = OK_A;
		return 0;
	case 2:
		o->a = *areg(x, reg);
		return 0;
	case 3:
		o->a = *areg(x, reg);
		*areg(x, reg) += inc;
		return 0;
	case 4:
		*areg(x, reg) -= inc;
		o->a = *areg(x, reg);
		return 0;
	case 5:
		if (fetch(x, &w))
			return -1;
		o->a = *areg(x, reg) + (short)w;
		return 0;
	case 6:
		if (fetch(x, &w) || (w & 0x100))
			return -1;
		o->a = *areg(x, reg) + (char)(w & 0xff) + xreg(x, w);
		return 0;
	}
	switch (reg) {
	case 0:
		if (fetch(x, &w))
			return -1;
		o->a = (short)w;
		return 0;
	case 1:
		if (fetch(x, &w) || fetch(x, &w2))
			return -1;
		o->a = w << 16 | w2;
		return 0;
	case 2:
		base = x->pc + x->len;
		if (fetch(x, &w))
			return -1;
		o->a = base + (short)w;
		return 0;
	case 3:
		base = x->pc + x->len;
		if (fetch(x, &w) || (w & 0x100))
			return -1;
		o->a = base + (char)(w & 0xff) + xreg(x, w);
		return 0;
	case 4:
		o->kind = OK_I;
		if (fetch(x, &w))
			return -1;
		if (sz == 4) {
			if (fetch(x, &w2))
				return -1;
			w = w << 16 | w2;
		} else if (sz == 1)
			w &= 0xff;
		o->a = w;
		return 0;
	}
	return -1;
}

/* ---- the address space: memory, aliases, registers ---- */

static int iorb(), iowb();

/* 1: a register window address, 0: memory at *na */
static int
where(a, na)
	unsigned long a, *na;
{
	unsigned long o = a & 0xffffff;

	*na = a;
	if (a >= 0xff000000) {
		if (o < tosc.t_ramsize || (o >= 0xe00000 && o < 0xf00000)) {
			*na = o;	/* the 24-bit alias of RAM or ROM */
			return 0;
		}
		return 1;
	}
	return a >= 0xf00000 && a < 0x1000000 && (a < 0xfa0000 || a >= 0xfc0000);
}

__asm__(".weak ds_bltgo");
extern void ds_bltgo();

/* the guest's blit, on the machine's blitter; without one it ends at once */
static void
bltgo()
{
	if (&ds_bltgo)
		ds_bltgo(tosc.t_blt);
	else
		tosc.t_blt[0x3c] &= 0x7f;
}

/* 0, or -1 with x->ba set */
static int
mget(x, a, sz, vp)
	struct ix *x;
	unsigned long a;
	int sz;
	unsigned long *vp;
{
	unsigned long na, v = 0;
	unsigned char b[4];
	int i;

	/* a blit cut short by a signal goes on when the guest looks */
	na = a & 0xffffff;
	if ((tosc.t_flags & TEF_FALCON) && na <= 0xff8a3c && na + sz > 0xff8a3c && (tosc.t_blt[0x3c] & 0x80))
		bltgo();
	if (!where(a, &na)) {
		if (copyin((caddr_t)na, (caddr_t)b, sz)) {
			x->ba = a;
			return -1;
		}
		for (i = 0; i < sz; i++)
			v = v << 8 | b[i];
	} else
		for (i = 0; i < sz; i++) {
			if (iorb(a + i, b)) {
				x->ba = a + i;
				return -1;
			}
			v = v << 8 | b[0];
		}
	*vp = v;
	return 0;
}

/* the display service, on a Falcon: the guest's Videl writes */
__asm__(".weak ds_vidput");
extern void ds_vidput();

static int
mput(x, a, sz, v)
	struct ix *x;
	unsigned long a, v;
	int sz;
{
	unsigned long na, v0 = v;
	unsigned char b[4];
	int i;

	for (i = sz - 1; i >= 0; i--, v >>= 8)
		b[i] = v;
	if (!where(a, &na)) {
		if (copyout((caddr_t)b, (caddr_t)na, sz)) {
			x->ba = a;
			return -1;
		}
		return 0;
	}
	for (i = 0; i < sz; i++)
		if (iowb(a + i, b[i])) {
			x->ba = a + i;
			return -1;
		}
	if ((tosc.t_flags & TEF_FALCON) && &ds_vidput)
		ds_vidput(a, sz, v0);
	na = a & 0xffffff;
	if ((tosc.t_flags & TEF_FALCON) && na <= 0xff8a3c && na + sz > 0xff8a3c && (tosc.t_blt[0x3c] & 0x80))
		bltgo();
	return 0;
}

static unsigned long
szmask(sz)
	int sz;
{
	return sz == 4 ? 0xffffffff : sz == 2 ? 0xffff : 0xff;
}

static int
oget(x, o, sz, vp)
	struct ix *x;
	struct opnd *o;
	int sz;
	unsigned long *vp;
{
	switch (o->kind) {
	case OK_D:
		*vp = GR_D(x->r, o->reg) & szmask(sz);
		return 0;
	case OK_A:
		*vp = *areg(x, o->reg) & szmask(sz);
		return 0;
	case OK_I:
		*vp = o->a;
		return 0;
	}
	return mget(x, (unsigned long)o->a, sz, vp);
}

static int
oput(x, o, sz, v)
	struct ix *x;
	struct opnd *o;
	int sz;
	unsigned long v;
{
	unsigned long m = szmask(sz);

	switch (o->kind) {
	case OK_D:
		GR_D(x->r, o->reg) = (GR_D(x->r, o->reg) & ~m) | (v & m);
		return 0;
	case OK_A:
		*areg(x, o->reg) = sz == 2 ? (short)v : v;
		return 0;
	case OK_I:
		return -2;
	}
	return mput(x, (unsigned long)o->a, sz, v & m);
}

static void
setcc(x, cc, keepx)
	struct ix *x;
	int cc, keepx;
{
	int m = keepx ? 0x0f : 0x1f;

	GR_SR(x->r) = (GR_SR(x->r) & ~m) | (cc & m);
}

static int
nz(v, sz)
	unsigned long v;
	int sz;
{
	v &= szmask(sz);
	return (v == 0 ? CC_Z : 0) | (v & (1L << (8 * sz - 1)) ? CC_N : 0);
}

/* d + s or d - s (sub), with the flags; returns the result */
static unsigned long
arith(x, d, s, sz, sub, cmp)
	struct ix *x;
	unsigned long d, s;
	int sz, sub, cmp;
{
	unsigned long m = szmask(sz), top = 1L << (8 * sz - 1), r;
	int cc;

	d &= m;
	s &= m;
	r = (sub ? d - s : d + s) & m;
	cc = nz(r, sz);
	if (sub) {
		if (s > d)
			cc |= CC_C | CC_X;
		if ((d ^ s) & (d ^ r) & top)
			cc |= CC_V;
	} else {
		if (r < d)
			cc |= CC_C | CC_X;
		if (~(d ^ s) & (d ^ r) & top)
			cc |= CC_V;
	}
	if (cmp)
		setcc(x, cc & 0x0f, 1);
	else
		setcc(x, cc, 0);
	return r;
}

static int
cond(x, c)
	struct ix *x;
	int c;
{
	int sr = GR_SR(x->r), C = sr & 1, V = sr >> 1 & 1, Z = sr >> 2 & 1, N = sr >> 3 & 1;

	switch (c) {
	case 0: return 1;
	case 1: return 0;
	case 2: return !C && !Z;
	case 3: return C || Z;
	case 4: return !C;
	case 5: return C;
	case 6: return !Z;
	case 7: return Z;
	case 8: return !V;
	case 9: return V;
	case 10: return !N;
	case 11: return N;
	case 12: return N == V;
	case 13: return N != V;
	case 14: return !Z && N == V;
	}
	return Z || N != V;
}

/* movem between registers and memory */
static int
movem(x, op)
	struct ix *x;
	unsigned long op;
{
	unsigned long mask, v;
	int sz = op & 0x40 ? 4 : 2, mode = (op >> 3) & 7, reg = op & 7, i, n;
	struct opnd o;
	long a;

	if (fetch(x, &mask))
		return -1;
	if (mode == 4) {			/* -(An): A7 first, downwards */
		a = *areg(x, reg);
		for (i = 0; i < 16; i++) {
			if (!(mask & (1L << i)))
				continue;
			n = 15 - i;
			a -= sz;
			v = n < 8 ? GR_D(x->r, n) : *areg(x, n - 8);
			if (mput(x, (unsigned long)a, sz, v & szmask(sz)))
				return -2;
		}
		*areg(x, reg) = a;
		return 0;
	}
	if (mode == 3)
		o.a = *areg(x, reg);
	else if (ea(x, mode, reg, sz, &o) || o.kind != OK_M)
		return -1;
	a = o.a;
	for (i = 0; i < 16; i++) {
		if (!(mask & (1L << i)))
			continue;
		if (op & 0x400) {		/* to registers, sign-extended */
			if (mget(x, (unsigned long)a, sz, &v))
				return -2;
			if (sz == 2)
				v = (short)v;
			if (i < 8)
				GR_D(x->r, i) = v;
			else
				*areg(x, i - 8) = v;
		} else {
			v = i < 8 ? GR_D(x->r, i) : *areg(x, i - 8);
			if (mput(x, (unsigned long)a, sz, v & szmask(sz)))
				return -2;
		}
		a += sz;
	}
	if (mode == 3)
		*areg(x, reg) = a;
	return 0;
}

static int
movep(x, op)
	struct ix *x;
	unsigned long op;
{
	unsigned long d, v = 0, b;
	long a;
	int n = op & 0x40 ? 4 : 2, i, dn = (op >> 9) & 7;

	if (fetch(x, &d))
		return -1;
	a = *areg(x, (int)op & 7) + (short)d;
	if (op & 0x80) {			/* register to memory */
		v = GR_D(x->r, dn);
		for (i = n - 1; i >= 0; i--, a += 2)
			if (mput(x, (unsigned long)a, 1, (v >> (8 * i)) & 0xff))
				return -2;
		return 0;
	}
	for (i = 0; i < n; i++, a += 2) {
		if (mget(x, (unsigned long)a, 1, &b))
			return -2;
		v = v << 8 | b;
	}
	if (n == 2)
		GR_D(x->r, dn) = (GR_D(x->r, dn) & ~0xffffL) | v;
	else
		GR_D(x->r, dn) = v;
	return 0;
}

/* btst/bchg/bclr/bset on <ea> with bit number bn */
static int
bitop(x, op, bn)
	struct ix *x;
	unsigned long op;
	int bn;
{
	struct opnd o;
	unsigned long v, bit;
	int kind = (op >> 6) & 3, sz;

	if (ea(x, (int)(op >> 3) & 7, (int)op & 7, 1, &o))
		return -1;
	sz = o.kind == OK_D ? 4 : 1;
	bit = 1L << (bn & (8 * sz - 1));
	if (oget(x, &o, sz, &v))
		return -2;
	GR_SR(x->r) = (GR_SR(x->r) & ~CC_Z) | (v & bit ? 0 : CC_Z);
	if (kind == 0)
		return 0;
	v = kind == 1 ? v ^ bit : kind == 2 ? v & ~bit : v | bit;
	return oput(x, &o, sz, v) ? -2 : 0;
}

/* one data instruction touching a register window: 0, -1 not handled, -2 bus error */
static int
emul(x)
	struct ix *x;
{
	unsigned long op, w, s, d;
	struct opnd so, o;
	int sz, mode, reg, dn, k;

	if (fetch(x, &op))
		return -1;
	mode = (op >> 3) & 7;
	reg = op & 7;
	dn = (op >> 9) & 7;
	sz = (op >> 6) & 3;
	sz = sz == 0 ? 1 : sz == 1 ? 2 : sz == 2 ? 4 : 0;

	switch (op >> 12) {
	case 1: case 2: case 3:			/* move, movea */
		sz = op >> 12 == 1 ? 1 : op >> 12 == 3 ? 2 : 4;
		if (ea(x, mode, reg, sz, &so) || oget(x, &so, sz, &s))
			return x->ba ? -2 : -1;
		if (((op >> 6) & 7) == 1) {
			*areg(x, dn) = sz == 2 ? (short)s : s;
			return 0;
		}
		if (ea(x, (int)(op >> 6) & 7, dn, sz, &o))
			return -1;
		if (oput(x, &o, sz, s))
			return -2;
		setcc(x, nz(s, sz), 1);
		return 0;

	case 0:
		if ((op & 0x138) == 0x108)
			return movep(x, op);
		if (op & 0x100)
			return bitop(x, op, (int)GR_D(x->r, dn));
		if ((op & 0xf00) == 0x800) {
			if (fetch(x, &w))
				return -1;
			return bitop(x, op, (int)w & 0xff);
		}
		k = dn;
		if (sz == 0 || k == 4 || k == 7 || (mode == 7 && reg == 4))
			return -1;
		if (ea(x, 7, 4, sz, &so) || ea(x, mode, reg, sz, &o))
			return -1;
		if (oget(x, &o, sz, &d))
			return -2;
		s = so.a;
		switch (k) {
		case 0: d |= s; break;
		case 1: d &= s; break;
		case 5: d ^= s; break;
		case 2: d = arith(x, d, s, sz, 1, 0); break;
		case 3: d = arith(x, d, s, sz, 0, 0); break;
		case 6: (void)arith(x, d, s, sz, 1, 1); return 0;
		}
		if (k == 0 || k == 1 || k == 5)
			setcc(x, nz(d, sz), 1);
		return oput(x, &o, sz, d) ? -2 : 0;

	case 4:
		if ((op & 0xfb80) == 0x4880 && mode >= 2)
			return movem(x, op);
		if (op == 0x4afc || sz == 0) {
			if ((op & 0xffc0) != 0x4ac0)
				return -1;
			if (ea(x, mode, reg, 1, &o) || oget(x, &o, 1, &d))	/* tas */
				return x->ba ? -2 : -1;
			setcc(x, nz(d, 1), 1);
			return oput(x, &o, 1, d | 0x80) ? -2 : 0;
		}
		switch (op & 0xff00) {
		case 0x4200:			/* clr */
			if (ea(x, mode, reg, sz, &o) || oput(x, &o, sz, 0L))
				return x->ba ? -2 : -1;
			setcc(x, CC_Z, 1);
			return 0;
		case 0x4a00:			/* tst */
			if (ea(x, mode, reg, sz, &o) || oget(x, &o, sz, &d))
				return x->ba ? -2 : -1;
			setcc(x, nz(d, sz), 1);
			return 0;
		case 0x4600:			/* not */
			if (ea(x, mode, reg, sz, &o) || oget(x, &o, sz, &d))
				return x->ba ? -2 : -1;
			d = ~d;
			setcc(x, nz(d, sz), 1);
			return oput(x, &o, sz, d) ? -2 : 0;
		case 0x4400:			/* neg */
			if (ea(x, mode, reg, sz, &o) || oget(x, &o, sz, &d))
				return x->ba ? -2 : -1;
			d = arith(x, 0L, d, sz, 1, 0);
			return oput(x, &o, sz, d) ? -2 : 0;
		}
		return -1;

	case 5:
		if (sz == 0) {			/* Scc; DBcc has no memory operand */
			if (mode == 1 || ea(x, mode, reg, 1, &o))
				return -1;
			return oput(x, &o, 1, cond(x, (int)(op >> 8) & 15) ? 0xffL : 0L) ? -2 : 0;
		}
		if (ea(x, mode, reg, sz, &o))
			return -1;
		s = dn ? dn : 8;
		if (o.kind == OK_A) {
			*areg(x, reg) += op & 0x100 ? -(long)s : (long)s;
			return 0;
		}
		if (oget(x, &o, sz, &d))
			return -2;
		d = arith(x, d, s, sz, (int)(op & 0x100), 0);
		return oput(x, &o, sz, d) ? -2 : 0;

	case 8: case 9: case 0xb: case 0xc: case 0xd:
		k = (op >> 6) & 7;
		if (k == 3 || k == 7) {		/* adda, suba, cmpa; mul, div not */
			if ((op >> 12) == 8 || (op >> 12) == 0xc)
				return -1;
			sz = k == 3 ? 2 : 4;
			if (ea(x, mode, reg, sz, &so) || oget(x, &so, sz, &s))
				return x->ba ? -2 : -1;
			if (sz == 2)
				s = (short)s;
			d = *areg(x, dn);
			if ((op >> 12) == 0xb)
				(void)arith(x, d, s, 4, 1, 1);
			else
				*areg(x, dn) = (op >> 12) == 9 ? d - s : d + s;
			return 0;
		}
		if (k >= 4 && mode < 2 && (op >> 12) != 0xb)
			return -1;		/* abcd, sbcd, addx, subx, exg */
		if (k >= 4 && mode == 1)
			return -1;		/* cmpm */
		if (ea(x, mode, reg, sz, &o))
			return -1;
		if (k < 4) {			/* <ea> op Dn -> Dn */
			if ((op >> 12) == 0xb && k >= 4)
				return -1;
			if (oget(x, &o, sz, &s))
				return -2;
			d = GR_D(x->r, dn);
			so.kind = OK_D;
			so.reg = dn;
		} else {			/* Dn op <ea> -> <ea> */
			s = GR_D(x->r, dn);
			if (oget(x, &o, sz, &d))
				return -2;
			so = o;
		}
		switch (op >> 12) {
		case 8: d |= s; break;
		case 0xc: d &= s; break;
		case 9: d = arith(x, d, s, sz, 1, 0); break;
		case 0xd: d = arith(x, d, s, sz, 0, 0); break;
		case 0xb:
			if (k < 4) {		/* cmp */
				(void)arith(x, d, s, sz, 1, 1);
				return 0;
			}
			d ^= s;			/* eor */
			break;
		}
		if ((op >> 12) == 8 || (op >> 12) == 0xc || (op >> 12) == 0xb)
			setcc(x, nz(d, sz), 1);
		return oput(x, &so, sz, d) ? -2 : 0;
	}
	return -1;
}

/* ---- vector 2 ---- */

/* bus error through the guest's vector 2, as a 68030 short bus fault frame */
int
tos_berr(gp, r, a, ssw, absent)
	struct guest_proc *gp;
	char *r;
	long a;
	int ssw, absent;
{
	char f[24];

	if (absent)
		tosc.t_st.ts_absent++;
	else
		tosc.t_st.ts_berr++;
	tosc.t_st.ts_refl[2]++;
	trace(absent ? "absent" : "bus error", a, GR_PC(r));
	bzero(f, sizeof f);
	P16(f + 2, guest_ssw030((ssw & ~7) | 5));	/* supervisor data */
	P32(f + 8, a);
	if (guest_reflect(gp, r, GR_PC(r), 0xa008, f, 26, -1) < 0) {
		printf("tos: bus error with no guest stack, pc %x\n", (int)GR_PC(r));
		psignal(curproc, SIGSEGV);
	}
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

/* writes a 68040 left in its write-back buffers (real 68040s complete them here) */
static int
writebacks(r)
	char *r;
{
	static char so[3] = { 14, 16, 18 }, ao[3] = { 24, 32, 40 };
	struct ix x;
	unsigned short *sp;
	unsigned long a, d;
	int i, n = 0, sz;

	x.r = r;
	for (i = 0; i < 3; i++) {
		sp = (unsigned short *)(r + 64 + so[i]);
		if (!(*sp & 0x80))
			continue;
		sz = (*sp >> 5) & 3;
		sz = sz == 1 ? 1 : sz == 2 ? 2 : 4;
		a = *(unsigned long *)(r + 64 + ao[i]);
		d = *(unsigned long *)(r + 64 + ao[i] + 4);
		(void)mput(&x, a, sz, d & szmask(sz));
		*sp &= ~0x80;
		n++;
	}
	return n;
}

int
tos_fault(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned long fa, na, d;
	int ssw, e, sz;
	struct ix x;
	char save[64];
	int sr;

	if ((gp->gp_flags & TGF_SOLO) || (e = guest_bfault(r, &fa, &ssw)) < 0)
		return 1;		/* a lone guest has no machine registers */
	if ((GR_FV(r) >> 12) == 7 && !where(fa, &na) &&
	    where(*(unsigned long *)(r + 64 + 8), &na))
		fa = *(unsigned long *)(r + 64 + 8);
	tosc.t_st.ts_lastpc = GR_PC(r);
	if (!where(fa, &na)) {
		if (as_segat(curproc->p_as, (caddr_t)fa) != 0)
			return 1;		/* paged memory: the stock handler */
		return tos_berr(gp, r, (long)fa, ssw, 0);
	}
	if (e)					/* instruction fetch */
		return tos_berr(gp, r, (long)fa, ssw, 0);
	x.r = r;
	x.ba = 0;
	if (guest_bfcycle(r, &d)) {		/* 68030: the faulted cycle, here */
		sz = (ssw >> 5) & 3;
		sz = sz ? sz : 4;
		if (ssw & 0x100 ? mget(&x, fa, sz, &d) : mput(&x, fa, sz, d & szmask(sz)))
			return tos_berr(gp, r, x.ba, ssw, 1);
		guest_bfdone(r, d);
		tosc.t_st.ts_io++;
		tos_kick(0);
		guest_trapret();
		return 0;
	}
	if (!(ssw & 0x100) && (GR_FV(r) >> 12) == 7 && writebacks(r)) {
		tosc.t_st.ts_io++;
		u.u_sigflag |= USTKCLEAR;
		tos_kick(0);
		guest_trapret();
		return 0;
	}
	x.pc = GR_PC(r);
	x.len = 0;
	bcopy(r, save, sizeof save);
	sr = GR_SR(r);
	e = emul(&x);
	if (e == 0) {
		tosc.t_st.ts_io++;
		GR_PC(r) = x.pc + x.len;
		u.u_sigflag |= USTKCLEAR;
		tos_kick(0);
		guest_trapret();
		return 0;
	}
	bcopy(save, r, sizeof save);
	GR_SR(r) = sr;
	if (e == -1) {
		trace("unhandled instruction at", x.pc, x.pc);
		return tos_berr(gp, r, (long)fa, ssw, 0);
	}
	return tos_berr(gp, r, x.ba ? x.ba : (long)fa, ssw, 1);
}

/* ---- 68030 MMU instructions (line F): recorded, never applied ---- */

int
tos_mmu(gp, r, op)
	struct guest_proc *gp;
	char *r;
	unsigned long op;
{
	struct ix x;
	struct opnd o;
	unsigned long ext, v, v2;
	unsigned long *p = 0;
	int sz = 4, rd;

	x.r = r;
	x.pc = GR_PC(r);
	x.len = 2;
	x.ba = 0;
	if (fetch(&x, &ext))
		return -1;
	rd = ext & 0x200;			/* register to memory */
	switch (ext >> 13) {
	case 0:					/* pmove TT0/TT1 */
		p = ((ext >> 10) & 7) == 2 ? &tosc.t_tt0 : ((ext >> 10) & 7) == 3 ? &tosc.t_tt1 : 0;
		break;
	case 2:					/* pmove TC/SRP/CRP */
		switch ((ext >> 10) & 7) {
		case 0: p = &tosc.t_tc; break;
		case 2: p = tosc.t_srp; sz = 8; break;
		case 3: p = tosc.t_crp; sz = 8; break;
		}
		break;
	case 3:					/* pmove MMUSR */
		if (ea(&x, (int)(op >> 3) & 7, (int)op & 7, 2, &o))
			return -1;
		if (rd)
			return oput(&x, &o, 2, (unsigned long)tosc.t_mmusr) ? -1 : x.len;
		if (oget(&x, &o, 2, &v))
			return -1;
		tosc.t_mmusr = v;
		return x.len;
	case 1:					/* pflush, pload */
		if (((ext >> 10) & 7) == 6 || (ext & 0xfde0) == 0x2000)
			if (ea(&x, (int)(op >> 3) & 7, (int)op & 7, 1, &o))
				return -1;
		return x.len;
	case 4:					/* ptest */
		if (ea(&x, (int)(op >> 3) & 7, (int)op & 7, 1, &o))
			return -1;
		tosc.t_mmusr = 0;
		return x.len;
	}
	if (p == 0 || ea(&x, (int)(op >> 3) & 7, (int)op & 7, sz, &o) ||
	    (o.kind != OK_M && sz == 8))
		return -1;
	if (rd) {
		if (oput(&x, &o, 4, p[0]))
			return -1;
		if (sz == 8 && mput(&x, (unsigned long)o.a + 4, 4, p[1]))
			return -1;
		return x.len;
	}
	if (oget(&x, &o, 4, &v) || (sz == 8 && mget(&x, (unsigned long)o.a + 4, 4, &v2)))
		return -1;
	p[0] = v;
	if (sz == 8)
		p[1] = v2;
	return x.len;
}

/* ---- MFP ---- */

static int mfpch[4] = { CH_TIMERA, CH_TIMERB, CH_TIMERC, CH_TIMERD };
static unsigned short presc[8] = { 0, 4, 10, 16, 50, 64, 100, 200 };

static int
hibit(v)
	unsigned int v;
{
	int n = -1;

	while (v) {
		v >>= 1;
		n++;
	}
	return n;
}

/* the channel the MFP would raise now, -1 none */
static int
mfp_next(m)
	struct tosmfp *m;
{
	int ch = hibit((unsigned int)(m->m_ipr & m->m_imr));

	if (ch < 0)
		return -1;
	if ((m->m_r[MFP_VR] & 8) && (m->m_isr >> ch))
		return -1;		/* in service: same and lower wait */
	return ch;
}

int
mfp_level()
{
	return mfp_next(&tosc.t_mfp) >= 0 ? 6 : 0;
}

/* interrupt acknowledge: the vector */
int
mfp_ack()
{
	struct tosmfp *m = &tosc.t_mfp;
	int ch = mfp_next(m), i;

	if (ch < 0)
		return 24;			/* spurious */
	m->m_ipr &= ~(1 << ch);
	if (m->m_r[MFP_VR] & 8)
		m->m_isr |= 1 << ch;
	tosc.t_st.ts_ints[ch]++;
	for (i = 0; i < 4; i++)
		if (mfpch[i] == ch && m->m_owed[i]) {
			m->m_owed[i]--;
			m->m_ipr |= 1 << ch;
		}
	return (m->m_r[MFP_VR] & 0xf0) | ch;
}

/* an acknowledge whose frame could not be written */
void
mfp_unack(vec)
	int vec;
{
	struct tosmfp *m = &tosc.t_mfp;
	int ch = vec & 15;

	if (vec == 24)
		return;
	m->m_isr &= ~(1 << ch);
	m->m_ipr |= 1 << ch;
	tosc.t_st.ts_ints[ch]--;
}

static void
mfp_irq(m, ch)
	struct tosmfp *m;
	int ch;
{
	if (m->m_ier & (1 << ch))
		m->m_ipr |= 1 << ch;
}

/* control value of timer i (0-7 delay mode), and its data register */
static int
tctl(m, i)
	struct tosmfp *m;
	int i;
{
	switch (i) {
	case 0: return m->m_r[MFP_TACR] & 0xf;
	case 1: return m->m_r[MFP_TBCR] & 0xf;
	case 2: return (m->m_r[MFP_TCDCR] >> 4) & 7;
	}
	return m->m_r[MFP_TCDCR] & 7;
}

/* each clock tick: timer periods of one MFP */
static void
mfp_tick(m)
	struct tosmfp *m;
{
	unsigned long per;
	int i, c, ch, n;

	for (i = 0; i < 4; i++) {
		c = tctl(m, i);
		ch = mfpch[i];
		if (c == 0 || c > 7 || !(m->m_ier & (1 << ch))) {
			m->m_acc[i] = 0;
			m->m_owed[i] = 0;
			continue;
		}
		per = presc[c] * (m->m_r[MFP_TADR + i] ? m->m_r[MFP_TADR + i] : 256);
		m->m_acc[i] += 2457600 / HZ;
		n = m->m_acc[i] / per;
		m->m_acc[i] %= per;
		n += m->m_owed[i];
		if (n > 0 && !(m->m_ipr & (1 << ch))) {
			m->m_ipr |= 1 << ch;
			n--;
		}
		m->m_owed[i] = n > 8 ? 8 : n;
	}
}

static void kmouse();

/* each clock tick: both MFPs' timers, a lost ACIA edge, the next button change */
void
tos_timers()
{
	struct tosmfp *m = &tosc.t_mfp;

	tosc.t_bsent = 0;
	if (tosc.t_bqn)
		kmouse();
	mfp_tick(m);
	mfp_tick(&tosc.t_mfp2);
	if (tosc.t_kcount && (tosc.t_kctl & 0x80) &&
	    !((m->m_ipr | m->m_isr) & (1 << CH_ACIA)))
		mfp_irq(m, CH_ACIA);
}

static int
mfp_rd(m, n, isst)
	struct tosmfp *m;
	int n, isst;
{
	int v;

	switch (n) {
	case MFP_GPIP:
		if (!isst)
			return m->m_r[n];
		v = 0xff;
		if (tosc.t_kcount && (tosc.t_kctl & 0x80))
			v &= ~0x10;
		if (tosc.t_fdcirq)
			v &= ~0x20;
		if (tosc.t_flags & TEF_MONO)
			v &= ~0x80;
		return v;
	case MFP_IERA: return m->m_ier >> 8;
	case MFP_IERB: return m->m_ier & 0xff;
	case MFP_IPRA: return m->m_ipr >> 8;
	case MFP_IPRB: return m->m_ipr & 0xff;
	case MFP_ISRA: return m->m_isr >> 8;
	case MFP_ISRB: return m->m_isr & 0xff;
	case MFP_IMRA: return m->m_imr >> 8;
	case MFP_IMRB: return m->m_imr & 0xff;
	}
	if (n >= MFP_TADR && n < MFP_TADR + 4) {
		int i = n - MFP_TADR;

		if (tctl(m, i) == 0)
			return m->m_cnt[i];
		/* running: count down one step per read */
		m->m_cnt[i] = m->m_cnt[i] > 1 ? m->m_cnt[i] - 1 : m->m_r[n];
		return m->m_cnt[i];
	}
	if (n == 21 || n == 22)			/* RSR, TSR: idle, buffer empty */
		return n == 22 ? 0x80 : 0;
	return m->m_r[n];
}

static void
mfp_wr(m, n, v)
	struct tosmfp *m;
	int n, v;
{
	unsigned short *p = 0;
	int hi = 0;

	m->m_r[n] = v;
	switch (n) {
	case MFP_IERA: hi = 1; /* FALLTHROUGH */
	case MFP_IERB:
		p = &m->m_ier;
		break;
	case MFP_IPRA: case MFP_IPRB:		/* writing 0 clears */
		m->m_ipr &= n == MFP_IPRA ? (v << 8 | 0xff) : (0xff00 | v);
		return;
	case MFP_ISRA: case MFP_ISRB:
		m->m_isr &= n == MFP_ISRA ? (v << 8 | 0xff) : (0xff00 | v);
		return;
	case MFP_IMRA: hi = 1; /* FALLTHROUGH */
	case MFP_IMRB:
		p = &m->m_imr;
		break;
	case MFP_VR:
		if (!(v & 8))
			m->m_isr = 0;
		return;
	default:
		if (n >= MFP_TADR && n < MFP_TADR + 4 && tctl(m, n - MFP_TADR) == 0)
			m->m_cnt[n - MFP_TADR] = v;
		if (n >= MFP_TACR && n <= MFP_TCDCR)
			m->m_acc[n - MFP_TACR] = 0;
		return;
	}
	*p = hi ? (*p & 0xff) | v << 8 : (*p & 0xff00) | v;
	if (p == &m->m_ier)
		m->m_ipr &= m->m_ier;
}

/* ---- IKBD ACIA ---- */

static void
kpush(b)
	int b;
{
	struct tosctr *t = &tosc;

	if (t->t_kcount >= ACIA_FIFO)
		return;
	t->t_kfifo[(t->t_khead + t->t_kcount) % ACIA_FIFO] = b;
	if (t->t_kcount++ == 0 && (t->t_kctl & 0x80))
		mfp_irq(&t->t_mfp, CH_ACIA);
}

/*
 * Mouse packets.  Queued button changes go out at most one a clock
 * tick, as soon as the FIFO has room, spaced as a keyboard would.
 * Pending motion goes out once the guest has read everything before,
 * so motion arriving meanwhile joins it.
 */
static void
kmouse()
{
	struct tosctr *t = &tosc;
	int dx, dy, i, b;

	while (t->t_kcount <= ACIA_FIFO - 3) {
		b = t->t_bqn && !t->t_bsent;
		if (!b && (t->t_kcount || (!t->t_mdx && !t->t_mdy)))
			return;
		if (b) {
			t->t_qbtn = t->t_bq[0];
			for (i = 1; i < t->t_bqn; i++)
				t->t_bq[i - 1] = t->t_bq[i];
			t->t_bqn--;
			t->t_bsent = 1;
		}
		dx = t->t_mdx > 127 ? 127 : t->t_mdx < -128 ? -128 : t->t_mdx;
		dy = t->t_mdy > 127 ? 127 : t->t_mdy < -128 ? -128 : t->t_mdy;
		kpush(0xf8 | t->t_qbtn);
		kpush(dx & 0xff);
		kpush(dy & 0xff);
		t->t_mdx -= dx;
		t->t_mdy -= dy;
		t->t_st.ts_kbin += 3;
	}
}

/* motion a stalled guest gets at once: a screen's width */
static int
clip(v)
	int v;
{
	return v > 1280 ? 1280 : v < -1280 ? -1280 : v;
}

void
tos_input(ti)
	struct tosinput *ti;
{
	struct tosctr *t = &tosc;
	unsigned char *b = ti->ti_b;
	int n = ti->ti_n, s = splhi_();

	t->t_st.ts_kbin += n;
	while (n-- > 0)
		kpush((int)*b++);
	t->t_mdx = clip(t->t_mdx + ti->ti_dx);
	t->t_mdy = clip(t->t_mdy + ti->ti_dy);
	if ((ti->ti_btn & 3) != t->t_mbtn) {
		t->t_mbtn = ti->ti_btn & 3;
		if (t->t_bqn == sizeof t->t_bq)
			t->t_bqn--;		/* full: the newest state replaces the last */
		t->t_bq[t->t_bqn++] = t->t_mbtn;
	}
	kmouse();
	splx_(s);
	tos_kick(1);
}

static int
bcd(v)
	int v;
{
	return (v / 10) << 4 | v % 10;
}

static void gmt();

/* parameter bytes of IKBD commands $07-$22; -1 unknown */
static char iklen[0x23] = {
	-1, -1, -1, -1, -1, -1, -1, 1, 0, 4, 2, 2, 2, 0, 5, 0,
	0, 0, 0, 0, 0, 0, 0, 1, 0, 6, 0, 6, 0, -1, -1, -1,
	3, 2, 2
};

static void
ikbd(v)
	int v;
{
	struct tosctr *t = &tosc;
	int c, i, tm[6];

	t->t_st.ts_kbcmd++;
	if (t->t_kcmdn == 0) {
		if (v == 0x80)
			t->t_kcmdlen = 1;
		else if (v >= 0x87 && v <= 0x9a)
			t->t_kcmdlen = 0;
		else if (v < sizeof iklen && iklen[v] >= 0)
			t->t_kcmdlen = iklen[v];
		else
			return;
	}
	t->t_kcmd[t->t_kcmdn++] = v;
	if (t->t_kcmdn <= t->t_kcmdlen && t->t_kcmdn < sizeof t->t_kcmd)
		return;
	t->t_kcmdn = 0;
	c = t->t_kcmd[0];
	switch (c) {
	case 0x80:
		if (t->t_kcmd[1] == 1)
			kpush(0xf1);
		break;
	case 0x1c:
		gmt(tm);
		kpush(0xfc);
		kpush(bcd(tm[0] % 100));
		for (i = 1; i < 6; i++)
			kpush(bcd(tm[i]));
		break;
	case 0x16:
		kpush(0xfd);
		kpush(0);
		kpush(0);
		break;
	case 0x0d:
		kpush(0xf7);
		for (i = 0; i < 5; i++)
			kpush(0);
		break;
	case 0x21:
		kpush(0xf6);
		for (i = 0; i < 7; i++)
			kpush(0);
		break;
	default:
		if (c >= 0x87) {
			kpush(0xf6);
			kpush(c & 0x7f);
			for (i = 0; i < 6; i++)
				kpush(0);
		}
	}
}

/* ---- RTC (MC146818), video, the rest ---- */

/* the wall clock (UTC): year, month, day, hour, minute, second */
static void
gmt(tm)
	int *tm;
{
	static char mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	long s = hrestime.tv_sec, d = s / 86400;
	int y = 1970, m = 0, n;

	s %= 86400;
	tm[3] = s / 3600;
	tm[4] = s / 60 % 60;
	tm[5] = s % 60;
	for (;;) {
		n = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) ? 366 : 365;
		if (d < n)
			break;
		d -= n;
		y++;
	}
	for (;;) {
		n = mdays[m] + (m == 1 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
		if (d < n)
			break;
		d -= n;
		m++;
	}
	tm[0] = y;
	tm[1] = m + 1;
	tm[2] = d + 1;
}

static int
rtc_rd()
{
	struct tosctr *t = &tosc;
	int i = t->t_rtcidx, tm[6], v;
	static char map[10] = { 5, -1, 4, -1, 3, -1, -1, 2, 1, 0 };

	if (i < 10) {
		if (map[i] < 0)
			return t->t_rtc[i];
		gmt(tm);
		if (map[i] == 0)
			tm[0] -= 1968;
		v = tm[(int)map[i]];
		return t->t_rtc[11] & 4 ? v : bcd(v);
	}
	switch (i) {
	case 10: return t->t_rtc[10] & 0x7f;
	case 12: return 0;
	case 13: return 0x80;
	}
	return t->t_rtc[i];
}

static unsigned short
st2tt(v)
	int v;
{
	int r = 0, k, n;

	for (k = 0; k < 12; k += 4) {
		n = (v >> k) & 15;
		r |= ((n & 7) << 1 | n >> 3) << k;
	}
	return r;
}

static unsigned short
tt2st(v)
	int v;
{
	int r = 0, k, n;

	for (k = 0; k < 12; k += 4) {
		n = (v >> k) & 15;
		r |= (n >> 1 | (n & 1) << 3) << k;
	}
	return r;
}

static void
vid_wr(o, v)
	int o, v;
{
	struct tosctr *t = &tosc;
	int i, j;
	unsigned short w;

	t->t_vid[o] = v;
	t->t_vgen++;
	if (o >= 0x40 && o < 0x60) {		/* ST palette -> TT palette bank */
		i = (o - 0x40) >> 1;
		w = st2tt(t->t_vid[0x40 + 2 * i] << 8 | t->t_vid[0x41 + 2 * i]);
		j = ((t->t_vid[0x63] & 15) * 16 + i) * 2;
		t->t_ttpal[j] = w >> 8;
		t->t_ttpal[j + 1] = w;
	} else if (o == 0x60)
		t->t_vid[0x62] = (t->t_vid[0x62] & ~7) | (v & 3);
	else if (o == 0x62 && (v & 7) <= 2)
		t->t_vid[0x60] = v & 3;
}

static void
ttpal_wr(o, v)
	int o, v;
{
	struct tosctr *t = &tosc;
	int e = o >> 1, i;
	unsigned short w;

	t->t_ttpal[o] = v;
	t->t_vgen++;
	if (e / 16 == (t->t_vid[0x63] & 15)) {
		i = e % 16;
		w = tt2st(t->t_ttpal[2 * e] << 8 | t->t_ttpal[2 * e + 1]);
		t->t_vid[0x40 + 2 * i] = w >> 8;
		t->t_vid[0x41 + 2 * i] = w;
	}
}

static int
vid_rd(o)
	int o;
{
	struct tosctr *t = &tosc;
	unsigned long a;

	if (o >= 5 && o <= 9 && (o & 1)) {	/* video address counter: moving */
		a = (t->t_vid[1] << 16 | t->t_vid[3] << 8 | t->t_vid[0x0d]) +
		    (t->t_vcnt++ % 400) * 80;
		return (a >> (8 * (4 - (o - 5) / 2 - 2))) & 0xff;
	}
	return t->t_vid[o];
}

/* FDC and ACSI DMA: floppies answer "no drive", hard disks never */
static int
dma_rd(o)
	int o;
{
	struct tosctr *t = &tosc;

	switch (o) {
	case 4:
		return 0;
	case 5:
		if (!(t->t_dma[7] & 0x18) && !(t->t_dma[7] & 6))
			t->t_fdcirq = 0;	/* status read ends the interrupt */
		return t->t_dma[7] & 0x10 ? 0 : t->t_dma[7] & 6 ? t->t_dma[0x8 + (t->t_dma[7] & 6)] : 0;
	case 6:
		return 0;
	case 7:
		return 1;			/* DMA OK */
	}
	return t->t_dma[o];
}

static void
dma_wr(o, v)
	int o, v;
{
	struct tosctr *t = &tosc;

	if (o == 5 && !(t->t_dma[7] & 0x18)) {
		if (t->t_dma[7] & 6)
			t->t_dma[0x8 + (t->t_dma[7] & 6)] = v;	/* track, sector, data */
		else {
			t->t_fdcirq = 1;	/* command done at once */
			mfp_irq(&t->t_mfp, CH_FDC);
		}
		return;
	}
	if (o != 5)
		t->t_dma[o] = v;
}

static int
scc_rd(o)
	int o;
{
	struct tosctr *t = &tosc;
	int ch = (o >> 2) & 1, p;

	if (o & 2)
		return 0;			/* data */
	p = t->t_sccptr[ch];
	t->t_sccptr[ch] = 0;
	return p == 0 ? 0x2c : p == 1 ? 0x01 : 0;
}

static void
scc_wr(o, v)
	int o, v;
{
	struct tosctr *t = &tosc;
	int ch = (o >> 2) & 1;

	if (o & 2)
		return;
	if (t->t_sccptr[ch] == 0)
		t->t_sccptr[ch] = (v & 7) | ((v & 0x38) == 0x08 ? 8 : 0);
	else
		t->t_sccptr[ch] = 0;
}

/*
 * Falcon registers: -2 not a Falcon one, -1 absent (no TT MFP, palette
 * or SCU).  The monitor type is the machine's own when it is a Falcon.
 */
__asm__(".weak ata_machtype");
extern unsigned long ata_machtype;

static int
falcon_rd(o)
	int o;
{
	struct tosctr *t = &tosc;
	int v, n;

	if (o == 0xff8006) {		/* bits 5, 4 and 1: ST-RAM 512 KB << n, 5 = 14 MB */
		v = &ata_machtype ? *(volatile unsigned char *)0xffff8006 : 0x80;
		for (n = 0; n < 4 && 0x80000 << (n + 1) <= t->t_ramsize; n++)
			;
		if (t->t_ramsize >= 0xe00000)
			n = 5;
		return (v & ~0x32) | (n & 6) << 3 | (n & 1) << 1;
	}
	if (o >= 0xff8200 && o < 0xff8300)
		return vid_rd(o & 0xff);
	if (o >= 0xff9800 && o < 0xff9c00)
		return t->t_fpal[o & 0x3ff];
	if (o >= 0xff8900 && o < 0xff8944)
		return t->t_snd[o - 0xff8900];
	if (o >= 0xff8a00 && o < 0xff8a40)
		return t->t_blt[o - 0xff8a00];
	if (o >= 0xffa200 && o < 0xffa208)	/* host port: transmit always empty */
		return o == 0xffa202 ? 6 : t->t_dsp[o & 7];
	if ((o >= 0xfffa80 && o < 0xfffab0) || (o >= 0xff8400 && o < 0xff8600) ||
	    (o >= 0xff8e00 && o < 0xff8e10))
		return -1;
	return -2;
}

static int
falcon_wr(o, v)
	int o, v;
{
	struct tosctr *t = &tosc;

	if (o == 0xff8006)
		return 0;
	if (o >= 0xff8200 && o < 0xff8300)
		vid_wr(o & 0xff, v);
	else if (o >= 0xff9800 && o < 0xff9c00) {
		t->t_fpal[o & 0x3ff] = v;
		t->t_vgen++;
	} else if (o >= 0xff8900 && o < 0xff8944)
		t->t_snd[o - 0xff8900] = v;
	else if (o >= 0xff8a00 && o < 0xff8a40)
		t->t_blt[o - 0xff8a00] = v;
	else if (o >= 0xffa200 && o < 0xffa208)
		t->t_dsp[o & 7] = v;
	else
		return falcon_rd(o) == -1 ? -1 : -2;
	return 0;
}

/*
 * One byte of the ST I/O page.  0, or -1: nothing there (bus error).
 */
static int
iorb(a, vp)
	unsigned long a;
	unsigned char *vp;
{
	struct tosctr *t = &tosc;
	int o = a & 0xffffff, v = 0xff;

	if ((t->t_flags & TEF_FALCON) && (v = falcon_rd(o)) != -2) {
		if (v < 0)
			goto absent;
	} else if (o >= 0xff8000 && o < 0xff8100)
		v = t->t_misc[o & 15];
	else if (o >= 0xff8200 && o < 0xff8266)	/* ST/TT shifter; no Videl above */
		v = vid_rd(o & 0xff);
	else if (o >= 0xff8400 && o < 0xff8600)
		v = t->t_ttpal[o & 0x1ff];
	else if (o >= 0xff8600 && o < 0xff8610)
		v = dma_rd(o & 15);
	else if (o >= 0xff8700 && o < 0xff8780)
		v = t->t_scsi[o & 15];
	else if (o >= 0xff8780 && o < 0xff8790)
		v = 0;				/* SCSI bus idle */
	else if (o >= 0xff8800 && o < 0xff8900)
		v = o & 2 ? 0xff : t->t_ym[t->t_ymsel & 15];
	else if (o == 0xff8961)
		v = t->t_rtcidx;
	else if (o == 0xff8963)
		v = rtc_rd();
	else if (o >= 0xff8960 && o < 0xff8970)
		v = 0xff;
	else if (o >= 0xff8c00 && o < 0xff8d00)
		v = o >= 0xff8c80 && o < 0xff8c88 ? scc_rd(o & 7) : 0;
	else if (o >= 0xff8e00 && o < 0xff8e10)
		v = t->t_scu[o & 15];
	else if (o == 0xff9200 || o == 0xff9201)
		v = o & 1 ? 0xff : 0x40;	/* TT DIP switches: no DMA sound, DD floppy */
	else if (o >= 0xfffa00 && o < 0xfffa30)
		v = o & 1 ? mfp_rd(&t->t_mfp, (o - 0xfffa01) >> 1, 1) : 0xff;
	else if (o >= 0xfffa80 && o < 0xfffab0)
		v = o & 1 ? mfp_rd(&t->t_mfp2, (o - 0xfffa81) >> 1, 0) : 0xff;
	else if (o == 0xfffc00)
		v = (t->t_kcount ? 1 : 0) | 2 | (t->t_kcount && (t->t_kctl & 0x80) ? 0x80 : 0);
	else if (o == 0xfffc02) {
		if (t->t_kcount) {
			t->t_kdata = t->t_kfifo[t->t_khead];
			t->t_khead = (t->t_khead + 1) % ACIA_FIFO;
			t->t_kcount--;
			t->t_st.ts_kbrd++;
			if (t->t_kcount == 0 || t->t_bqn)
				kmouse();
			/* the next byte is a new edge; none left, no request */
			if (t->t_kcount && (t->t_kctl & 0x80))
				mfp_irq(&t->t_mfp, CH_ACIA);
			else
				t->t_mfp.m_ipr &= ~(1 << CH_ACIA);
		}
		v = t->t_kdata;
	} else if (o == 0xfffc04)
		v = 2;
	else if (o >= 0xfffc00 && o < 0xfffc08)
		v = 0xff;
	else {
absent:
		t->t_st.ts_lastio = a;
		t->t_st.ts_lastiopc = t->t_st.ts_lastpc;
		return -1;
	}
	*vp = v;
	return 0;
}

static int
iowb(a, v)
	unsigned long a;
	int v;
{
	struct tosctr *t = &tosc;
	int o = a & 0xffffff, i;

	v &= 0xff;
	if ((t->t_flags & TEF_FALCON) && (i = falcon_wr(o, v)) != -2) {
		if (i < 0)
			goto absent;
	} else if (o >= 0xff8000 && o < 0xff8100)
		t->t_misc[o & 15] = v;
	else if (o >= 0xff8200 && o < 0xff8266)
		vid_wr(o & 0xff, v);
	else if (o >= 0xff8400 && o < 0xff8600)
		ttpal_wr(o & 0x1ff, v);
	else if (o >= 0xff8600 && o < 0xff8610)
		dma_wr(o & 15, v);
	else if (o >= 0xff8700 && o < 0xff8790)
		t->t_scsi[o & 15] = v;
	else if (o >= 0xff8800 && o < 0xff8900) {
		if (o & 2)
			t->t_ym[t->t_ymsel & 15] = v;
		else
			t->t_ymsel = v;
	} else if (o == 0xff8961)
		t->t_rtcidx = v & 63;
	else if (o == 0xff8963) {
		if (t->t_rtcidx >= 10)
			t->t_rtc[t->t_rtcidx] = v;
	} else if (o >= 0xff8960 && o < 0xff8970)
		;
	else if (o >= 0xff8c00 && o < 0xff8d00) {
		if (o >= 0xff8c80 && o < 0xff8c88)
			scc_wr(o & 7, v);
	} else if (o >= 0xff8e00 && o < 0xff8e10)
		t->t_scu[o & 15] = v;
	else if (o >= 0xfffa00 && o < 0xfffa30) {
		if (o & 1)
			mfp_wr(&t->t_mfp, (o - 0xfffa01) >> 1, v);
	} else if (o >= 0xfffa80 && o < 0xfffab0) {
		if (o & 1)
			mfp_wr(&t->t_mfp2, (o - 0xfffa81) >> 1, v);
	} else if (o == 0xfffc00) {
		if ((v & 3) == 3) {
			t->t_kcount = 0;
			t->t_kcmdn = 0;
		}
		t->t_kctl = v;
		if (t->t_kcount && (v & 0x80))
			mfp_irq(&t->t_mfp, CH_ACIA);
	} else if (o == 0xfffc02)
		ikbd(v);
	else if (o >= 0xfffc00 && o < 0xfffc08)
		;
	else {
absent:
		t->t_st.ts_lastio = a;
		t->t_st.ts_lastiopc = t->t_st.ts_lastpc;
		return -1;
	}
	return 0;
}

/* registers after a reset of the container */
void
tos_devinit(t)
	struct tosctr *t;
{
	ntrace = 0;
	t->t_scu[9] = 1;		/* memory controller configured: warm boot */
	t->t_vid[0x60] = t->t_flags & TEF_MONO ? 2 : 0;
	t->t_vid[0x62] = t->t_flags & TEF_MONO ? 6 : 4;
	t->t_rtc[11] = 0x06;		/* binary, 24-hour */
	t->t_kctl = 0x96;
}
