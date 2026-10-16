/*
 * nextfault.c -- instructions that reach the board's registers.
 *
 * The I/O pages are not mapped, so an access there faults; the
 * instruction is decoded and performed against the register model in
 * nextdev.c, and the guest continues after it.  Addresses nothing
 * backs read as zero and drop writes.
 *
 * K&R C.
 */

#include "next.h"
#include "../../next/hfswire.h"

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

/* 1: a register or an address nothing backs on the local bus, 0: memory */
int
nx_dev(a, wr)
	unsigned long a;
	int wr;
{
	if (a >= NX_LIMIT)
		return (a & 0xffff0000UL) == 0x820c0000UL;
	if (a >= NX_RAM + nx.ramsize && a < NX_RAM + NX_BANK && nx.ramsize)
		return 1;
	if (a < NX_ROM + NX_ROM_SIZE || (a >= NX_ROM2 && a < NX_ROM2 + NX_ROM_SIZE))
		return wr;
	if (a >= NX_RAM && a < NX_RAM + NX_BANK)
		return 0;
	if (a >= NX_VRAM && a < NX_VRAM + NX_VRAM_SIZE)
		return 0;
	if (a >= HF_BASE && a < HF_BASE + HF_SIZE)
		return 0;
	return 1;
}

/* the guest physical address of a; one that does not translate is memory, so its fault reaches the guest */
static unsigned long
phys(a, wr)
	unsigned long a;
	int wr;
{
	unsigned long pa;

	if (!nx.gp)
		return a;
	return guest_mmu_xlate(nx.gp, a, wr ? 2 : 1, &pa) ? NX_RAM : pa;
}

/* the memory is mirrored through its bank, as the memory sizing expects */
static unsigned long
alias(a)
	unsigned long a;
{
	if (a >= NX_RAM + nx.ramsize && a < NX_RAM + NX_BANK && nx.ramsize)
		return NX_RAM + ((a - NX_RAM) & (nx.ramsize - 1));
	return a;
}

/* 0, or -1 with x->ba set */
static int
mget(x, a, sz, vp)
	struct ix *x;
	unsigned long a;
	int sz;
	unsigned long *vp;
{
	unsigned long v = 0, pa;
	unsigned char b[4];
	int i;

	pa = phys(a, 0);
	a = pa == a ? alias(a) : a;
	pa = alias(pa);
	if (!nx_dev(pa, 0)) {
		if (copyin((caddr_t)a, (caddr_t)b, sz)) {
			x->ba = a;
			return -1;
		}
		for (i = 0; i < sz; i++)
			v = v << 8 | b[i];
	} else if (nx_read(pa, sz, &v)) {
		x->ba = a;
		return -1;
	}
	*vp = v;
	return 0;
}

static int
mput(x, a, sz, v)
	struct ix *x;
	unsigned long a, v;
	int sz;
{
	unsigned long pa = phys(a, 1);
	unsigned char b[4];
	int i;

	a = pa == a ? alias(a) : a;
	pa = alias(pa);
	if (nx_dev(pa, 1))
		return nx_write(pa, sz, v) ? (x->ba = a, -1) : 0;
	for (i = sz - 1; i >= 0; i--, v >>= 8)
		b[i] = v;
	if (copyout((caddr_t)b, (caddr_t)a, sz)) {
		x->ba = a;
		return -1;
	}
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

/* bit-field instructions on memory */
static int
bitfield(x, op)
	struct ix *x;
	unsigned long op;
{
	struct opnd o;
	unsigned long w, f = 0, mask;
	unsigned char b[5];
	long off;
	int dn, width, bo, n, i, k, kind = (op >> 8) & 7, cc;

	if ((op & 0xf8c0) != 0xe8c0 || ((op >> 3) & 7) < 2 || fetch(x, &w))
		return -1;
	dn = (w >> 12) & 7;
	off = w & 0x800 ? (long)GR_D(x->r, (w >> 6) & 7) : (long)((w >> 6) & 31);
	width = w & 0x20 ? (int)(GR_D(x->r, w & 7) & 31) : (int)(w & 31);
	if (width == 0)
		width = 32;
	if (ea(x, (int)(op >> 3) & 7, (int)op & 7, 1, &o) || o.kind != OK_M)
		return -1;
	o.a += off >> 3;
	bo = (int)(off & 7);
	n = (bo + width + 7) / 8;
	for (i = 0; i < n; i++) {
		unsigned long v;

		if (mget(x, o.a + i, 1, &v))
			return -2;
		b[i] = v;
	}
	for (i = 0; i < width; i++) {
		k = bo + i;
		f = (f << 1) | ((b[k >> 3] >> (7 - (k & 7))) & 1);
	}
	mask = width == 32 ? 0xffffffffUL : (1UL << width) - 1;
	if (kind == 7)
		f = GR_D(x->r, dn) & mask;
	cc = (f == 0 ? CC_Z : 0) | ((f >> (width - 1)) & 1 ? CC_N : 0);
	setcc(x, cc, 1);
	switch (kind) {
	case 1:
		GR_D(x->r, dn) = f;
		return 0;
	case 3:
		GR_D(x->r, dn) = (f >> (width - 1)) & 1 ? (long)(f | ~mask) : (long)f;
		return 0;
	case 5:
		for (i = 0; i < width && !((f >> (width - 1 - i)) & 1); i++)
			;
		GR_D(x->r, dn) = off + i;
		return 0;
	case 0:
		return 0;
	case 2: f = ~f & mask; break;
	case 4: f = 0; break;
	case 6: f = mask; break;
	}
	for (i = 0; i < width; i++) {
		k = bo + i;
		if ((f >> (width - 1 - i)) & 1)
			b[k >> 3] |= 1 << (7 - (k & 7));
		else
			b[k >> 3] &= ~(1 << (7 - (k & 7)));
	}
	for (i = 0; i < n; i++)
		if (mput(x, o.a + i, 1, b[i]))
			return -2;
	return 0;
}

/* MOVE16: one aligned 16-byte line, to or from a register window */
static int
move16(x, op)
	struct ix *x;
	unsigned long op;
{
	unsigned long w, w2, src, dst, v[4];
	int i, ax = op & 7, ay = 0, form = (op >> 3) & 1;

	if ((op & 0xffc0) != 0xf600)
		return -1;
	if (op & 0x20) {			/* (Ax)+,(Ay)+ */
		if (fetch(x, &w))
			return -1;
		ay = (w >> 12) & 7;
		src = *areg(x, ax) & ~15UL;
		dst = *areg(x, ay) & ~15UL;
	} else {
		if (fetch(x, &w) || fetch(x, &w2))
			return -1;
		src = dst = (w << 16 | w2) & ~15UL;
		if (form & 1)
			dst = *areg(x, ax) & ~15UL;
		else
			src = *areg(x, ax) & ~15UL;
	}
	for (i = 0; i < 4; i++)
		if (mget(x, src + 4 * i, 4, &v[i]))
			return -2;
	for (i = 0; i < 4; i++)
		if (mput(x, dst + 4 * i, 4, v[i]))
			return -2;
	if (op & 0x20) {
		*areg(x, ax) += 16;
		*areg(x, ay) += 16;
	} else if (!(op & 0x10))
		*areg(x, ax) += 16;
	return 0;
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

	case 0xe:				/* bit fields */
		return bitfield(x, op);

	case 0xf:				/* move16 */
		return move16(x, op);

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

/* the writes a 68040 left in its write-back buffers; WB1 holds bus-lane data */
static int
writebacks(x, r)
	struct ix *x;
	char *r;
{
	static int status[3] = { 82, 80, 78 }, address[3] = { 104, 96, 88 };
	unsigned long a, value;
	int i, w, size, shift, n = 0;

	for (i = 0; i < 3; i++) {
		w = G16(r + status[i]);
		if (!(w & 0x80))
			continue;
		size = (w >> 5) & 3;
		size = size == 0 ? 4 : size == 1 ? 1 : 2;
		a = G32(r + address[i]);
		value = G32(r + address[i] + 4);
		if (i == 0) {
			shift = (a & 3) * 8;
			if (size == 1)
				value >>= 24 - shift;
			else {
				if (size == 2)
					shift = (shift + 16) & 31;
				if (shift)
					value = ((value << shift) | (value >> (32 - shift))) & 0xffffffffUL;
			}
		}
		if (mput(x, a, size, value & szmask(size)))
			return -1;
		P16(r + status[i], w & ~0x80);
		n++;
	}
	return n;
}

/* 0: done, 1: not ours (the stock handler) */
int
nx_fault(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned long fa;
	int ssw, e, wr, sr, fmt, cm, wb;
	struct ix x;
	char save[64];

	/* the host's own code, such as its signal handlers, keeps its signals */
	if ((nx.flags & NXF_HOST) && (unsigned long)GR_PC(r) >= NX_LIMIT)
		return 1;
	fmt = GR_FV(r) >> 12;
	if ((fmt != 7 && fmt != 4) || (e = guest_bfault(r, &fa, &ssw)) < 0)
		return guest_fnote(gp, r, v);
	wr = !(ssw & 0x100);
	if (!nx_dev(phys(fa, wr), wr))
		return guest_fnote(gp, r, v);
	if (e)
		return nx_badaccess(gp, r, fa, "fetch");
	x.r = r;
	x.ba = 0;
	/* a faulted movem restarts from its first transfer: all of it runs here */
	cm = ssw & 0x1000;
	wb = fmt == 7 && !cm && wr && (G16(r + 78) | G16(r + 80) | G16(r + 82)) & 0x80;
	if (wb) {
		if (writebacks(&x, r) < 0)
			return nx_badaccess(gp, r, x.ba, "write");
	} else {
		x.pc = GR_PC(r);
		x.len = 0;
		bcopy(r, save, sizeof save);
		sr = GR_SR(r);
		if ((e = emul(&x)) != 0) {
			bcopy(save, r, sizeof save);
			GR_SR(r) = sr;
			return nx_badaccess(gp, r, x.ba ? x.ba : fa, e == -1 ? "instruction" : "access");
		}
		GR_PC(r) = x.pc + x.len;
		if (fmt == 7 && (cm || (G16(r + 78) | G16(r + 80) | G16(r + 82)) & 0x80)) {
			P16(r + 76, ssw & ~0x1000);
			P16(r + 78, G16(r + 78) & ~0x80);
			P16(r + 80, G16(r + 80) & ~0x80);
			P16(r + 82, G16(r + 82) & ~0x80);
		}
	}
	u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}
