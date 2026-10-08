/*
 * x87.c -- the 80387 floating-point unit, on the host's floating point.
 *
 * The 68k always has floating point: the 68881/68882 or the 040's FPU,
 * and where there is none (the Falcon) AMIX's kernel emulates it from
 * the F-line trap.  So this is plain C: the registers are long double
 * (the 68881's extended format has the x87's 64-bit significand),
 * arithmetic is the compiler's, the transcendental functions are
 * libm's (double).  Programs find a coprocessor (GetWinFlags
 * WF_80x87) and run their x87 instructions; WIN87EM passes through.
 *
 * Exceptions stay masked as Windows leaves them: results are infinities
 * and NaNs, the status word records them.  Stack faults set IE and SF.
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "x86.h"

typedef long double real;

struct x87 {
	real	st[8];		/* physical registers */
	int	top;
	u16	cw, sw;		/* sw without TOP */
	u16	tag;		/* 2 bits a physical register: 0 valid, 1 zero, 2 special, 3 empty */
	u16	ipsel, opsel;
	u32	ip, op;
	u16	fop;
};

#define	X	((struct x87 *)c->x87)
#define	ST(i)	(X->st[(X->top + (i)) & 7])
#define	PHYS(i)	((X->top + (i)) & 7)

#define	SW_IE	0x0001
#define	SW_DE	0x0002
#define	SW_ZE	0x0004
#define	SW_OE	0x0008
#define	SW_UE	0x0010
#define	SW_PE	0x0020
#define	SW_SF	0x0040
#define	SW_ES	0x0080
#define	SW_C0	0x0100
#define	SW_C1	0x0200
#define	SW_C2	0x0400
#define	SW_C3	0x4000
#define	SW_CC	(SW_C0 | SW_C1 | SW_C2 | SW_C3)

static real indefinite;	/* the default NaN */

static int
isnanr(v)
	real v;
{
	return v != v;
}

static int
isinfr(v)
	real v;
{
	return !isnanr(v) && v - v != v - v;
}

/* v * 2^e, exactly (in steps the format holds) */
static real
scale(v, e)
	real v;
	int e;
{
	int k;

	while (e > 0) {
		k = e > 30 ? 30 : e;
		v *= (real)((u32)1 << k);
		e -= k;
	}
	while (e < 0) {
		k = -e > 30 ? 30 : -e;
		v /= (real)((u32)1 << k);
		e += k;
	}
	return v;
}

/* v = m * 2^e with 0.5 <= |m| < 1 (v finite, not 0) */
static real
split(v, ep)
	real v;
	int *ep;
{
	int e = 0;
	real a = v < 0 ? -v : v;

	while (a >= 1152921504606846976.0L) {
		a /= 1152921504606846976.0L;
		e += 60;
	}
	while (a < 1.0L / 1152921504606846976.0L) {
		a *= 1152921504606846976.0L;
		e -= 60;
	}
	while (a >= 1.0L) {
		a /= 2;
		e++;
	}
	while (a < 0.5L) {
		a *= 2;
		e--;
	}
	*ep = e;
	return v < 0 ? -a : a;
}

static real
trunc_r(v)
	real v;
{
	real hi, lo, t;

	if (isnanr(v) || isinfr(v))
		return v;
	t = v < 0 ? -v : v;
	if (t >= 18446744073709551616.0L)	/* 2^64: no fraction left */
		return v;
	hi = (real)(u32)(t / 4294967296.0L);
	lo = t - hi * 4294967296.0L;
	lo = (real)(u32)lo;
	t = hi * 4294967296.0L + lo;
	return v < 0 ? -t : t;
}

/* to an integer by the control word's rounding */
static real
round_r(c, v)
	struct x86 *c;
	real v;
{
	real t = trunc_r(v), f = v - t;

	switch ((X->cw >> 10) & 3) {
	case 0:			/* nearest, ties to even */
		if (f > 0.5L || (f == 0.5L && fmod((double)t, 2.0) != 0))
			t += 1;
		else if (f < -0.5L || (f == -0.5L && fmod((double)t, 2.0) != 0))
			t -= 1;
		break;
	case 1:			/* down */
		if (f < 0)
			t -= 1;
		break;
	case 2:			/* up */
		if (f > 0)
			t += 1;
		break;
	}
	if (f != 0)
		X->sw |= SW_PE;
	return t;
}

static int
tagof(v)
	real v;
{
	if (v == 0)
		return 1;
	if (isnanr(v) || isinfr(v))
		return 2;
	return 0;
}

static void
push(c, v)
	struct x86 *c;
	real v;
{
	int p = (X->top - 1) & 7;

	if (((X->tag >> (2 * p)) & 3) != 3) {
		X->sw |= SW_IE | SW_SF | SW_C1;		/* overflow */
		v = indefinite;
	}
	X->top = p;
	X->st[p] = v;
	X->tag = (X->tag & ~(3 << (2 * p))) | tagof(v) << (2 * p);
}

static void
pop(c)
	struct x86 *c;
{
	X->tag |= 3 << (2 * X->top);
	X->top = (X->top + 1) & 7;
}

static int
empty(c, i)
	struct x86 *c;
	int i;
{
	return ((X->tag >> (2 * PHYS(i))) & 3) == 3;
}

/* ST(i), an empty one an underflow giving the NaN */
static real
get(c, i)
	struct x86 *c;
	int i;
{
	if (empty(c, i)) {
		X->sw |= SW_IE | SW_SF;
		X->sw &= ~SW_C1;
		return indefinite;
	}
	return ST(i);
}

static void
set(c, i, v)
	struct x86 *c;
	int i;
	real v;
{
	int p = PHYS(i);

	X->st[p] = v;
	X->tag = (X->tag & ~(3 << (2 * p))) | tagof(v) << (2 * p);
}

/* ---- memory formats ---- */

static int bigend = -1;

static real
ld32(u)
	u32 u;
{
	float f;

	memcpy(&f, &u, 4);
	return f;
}

static u32
st32(v)
	real v;
{
	float f = (float)v;
	u32 u;

	memcpy(&u, &f, 4);
	return u;
}

static real
ld64(lo, hi)
	u32 lo, hi;
{
	union { double d; u32 w[2]; } x;

	if (bigend < 0) {
		x.d = 1.0;
		bigend = x.w[0] != 0;
	}
	x.w[bigend ? 0 : 1] = hi;
	x.w[bigend ? 1 : 0] = lo;
	return x.d;
}

static void
st64(v, lo, hi)
	real v;
	u32 *lo, *hi;
{
	union { double d; u32 w[2]; } x;

	if (bigend < 0) {
		x.d = 1.0;
		bigend = x.w[0] != 0;
	}
	x.d = (double)v;
	*hi = x.w[bigend ? 0 : 1];
	*lo = x.w[bigend ? 1 : 0];
}

/* the x87's own 80-bit format: significand (with its integer bit), sign and 15-bit exponent */
static real
ld80(lo, hi, se)
	u32 lo, hi;
	int se;
{
	int e = se & 0x7fff;
	real m = (real)hi * 4294967296.0L + (real)lo, v;

	if (e == 0x7fff) {
		if ((hi & 0x7fffffff) || lo)
			return indefinite;
		v = 1.0L / 0.0L;
		return se & 0x8000 ? -v : v;
	}
	if (m == 0)
		v = 0;
	else
		v = scale(m, (e ? e : 1) - 16383 - 63);
	return se & 0x8000 ? -v : v;
}

static void
st80(v, lo, hi, se)
	real v;
	u32 *lo, *hi;
	int *se;
{
	int e, s = 0;
	real m;

	if (v < 0 || (v == 0 && 1.0L / v < 0)) {
		s = 0x8000;
		v = -v;
	}
	if (isnanr(v)) {
		*hi = 0xc0000000;
		*lo = 0;
		*se = 0xffff;
		return;
	}
	if (isinfr(v)) {
		*hi = 0x80000000;
		*lo = 0;
		*se = s | 0x7fff;
		return;
	}
	if (v == 0) {
		*hi = *lo = 0;
		*se = s;
		return;
	}
	m = split(v, &e);		/* v = m * 2^e, m in [0.5, 1) */
	e += 16382;
	m *= 18446744073709551616.0L;	/* the 64-bit significand */
	if (e <= 0) {			/* denormal */
		m = scale(m, e - 1);
		e = 0;
	}
	*hi = (u32)(m / 4294967296.0L);
	*lo = (u32)(m - (real)*hi * 4294967296.0L);
	*se = s | e;
}

static real
ldint(lo, hi, bits)
	u32 lo, hi;
	int bits;
{
	if (bits == 16)
		return (real)(short)lo;
	if (bits == 32)
		return (real)(s32)lo;
	if (hi & 0x80000000) {
		/* negative: the two's complement's magnitude */
		lo = ~lo + 1;
		hi = ~hi + (lo == 0);
		return -((real)hi * 4294967296.0L + (real)lo);
	}
	return (real)hi * 4294967296.0L + (real)lo;
}

/* to an integer of bits, by the rounding mode; out of range: the integer indefinite */
static void
stint(c, v, bits, lo, hi)
	struct x86 *c;
	real v;
	int bits;
	u32 *lo, *hi;
{
	real t, max = bits == 16 ? 32768.0L : bits == 32 ? 2147483648.0L : 9223372036854775808.0L;
	int neg;

	*hi = 0;
	if (isnanr(v) || (t = round_r(c, v)) >= max || t < -max) {
		X->sw |= SW_IE;
		*lo = bits == 16 ? 0x8000 : bits == 32 ? 0x80000000 : 0;
		*hi = bits == 64 ? 0x80000000 : 0;
		return;
	}
	neg = t < 0;
	if (neg)
		t = -t;
	*hi = (u32)(t / 4294967296.0L);
	*lo = (u32)(t - (real)*hi * 4294967296.0L);
	if (neg) {
		*lo = ~*lo + 1;
		*hi = ~*hi + (*lo == 0);
	}
	if (bits < 64)
		*hi = 0;
}

/* ---- comparing ---- */

static void
compare(c, a, b, unordered_ok)
	struct x86 *c;
	real a, b;
	int unordered_ok;
{
	X->sw &= ~SW_CC;
	if (isnanr(a) || isnanr(b)) {
		X->sw |= SW_C0 | SW_C2 | SW_C3;
		if (!unordered_ok)
			X->sw |= SW_IE;
	} else if (a < b)
		X->sw |= SW_C0;
	else if (a == b)
		X->sw |= SW_C3;
}

/* the arithmetic of D8/DC/DE and their integer forms: reg 0 add, 1 mul, 4 sub, 5 subr, 6 div, 7 divr */
static real
arith(c, reg, a, b)
	struct x86 *c;
	int reg;
	real a, b;
{
	real r;

	switch (reg) {
	case 0: r = a + b; break;
	case 1: r = a * b; break;
	case 4: r = a - b; break;
	case 5: r = b - a; break;
	case 6:
		if (b == 0 && a == a && a != 0 && !isinfr(a))
			X->sw |= SW_ZE;
		r = a / b;
		break;
	default:
		if (a == 0 && b == b && b != 0 && !isinfr(b))
			X->sw |= SW_ZE;
		r = b / a;
		break;
	}
	if (isnanr(r) && !isnanr(a) && !isnanr(b))
		X->sw |= SW_IE;
	return r;
}

/* ---- the environment ---- */

static void
stenv(c, sr, off)
	struct x86 *c;
	int sr;
	u32 off;
{
	int i, t;
	u16 tw = 0;

	/* the tag word as stored: each register looked at again */
	for (i = 0; i < 8; i++) {
		t = (X->tag >> (2 * i)) & 3;
		if (t != 3)
			t = tagof(X->st[i]);
		tw |= t << (2 * i);
	}
	if (c->o32) {
		x86_wr(c, sr, off, 2, X->cw | 0xffff0000);
		x86_wr(c, sr, off + 4, 2, (X->sw & ~0x3800) | (X->top << 11) | 0xffff0000);
		x86_wr(c, sr, off + 8, 2, tw | 0xffff0000);
		x86_wr(c, sr, off + 12, 2, X->ip);
		x86_wr(c, sr, off + 16, 2, X->ipsel | (u32)X->fop << 16);
		x86_wr(c, sr, off + 20, 2, X->op);
		x86_wr(c, sr, off + 24, 2, X->opsel | 0xffff0000);
	} else {
		x86_wr(c, sr, off, 1, X->cw);
		x86_wr(c, sr, off + 2, 1, (X->sw & ~0x3800) | (X->top << 11));
		x86_wr(c, sr, off + 4, 1, tw);
		x86_wr(c, sr, off + 6, 1, X->ip);
		x86_wr(c, sr, off + 8, 1, X->ipsel);
		x86_wr(c, sr, off + 10, 1, X->op);
		x86_wr(c, sr, off + 12, 1, X->opsel);
	}
}

static void
ldenv(c, sr, off)
	struct x86 *c;
	int sr;
	u32 off;
{
	int k = c->o32 ? 4 : 2, s;

	X->cw = x86_rd(c, sr, off, 1);
	s = x86_rd(c, sr, off + k, 1);
	X->top = (s >> 11) & 7;
	X->sw = s & ~0x3800;
	X->tag = x86_rd(c, sr, off + 2 * k, 1);
}

/* ---- the instructions ---- */

void
x87_init(c)
	struct x86 *c;
{
	if (!c->x87)
		c->x87 = calloc(1, sizeof(struct x87));
	indefinite = -(0.0L / 0.0L);
	X->cw = 0x037f;
	X->sw = 0;
	X->top = 0;
	X->tag = 0xffff;
}

static real
memreal(c, op, seg, off)
	struct x86 *c;
	int op, seg;
	u32 off;
{
	switch (op) {
	case 0xd8: return ld32(x86_rd(c, seg, off, 2));
	case 0xdc: return ld64(x86_rd(c, seg, off, 2), x86_rd(c, seg, off + 4, 2));
	case 0xda: return ldint(x86_rd(c, seg, off, 2), 0, 32);
	default: return ldint(x86_rd(c, seg, off, 1), 0, 16);	/* DE */
	}
}

static void
constant(c, k)
	struct x86 *c;
	int k;
{
	static real v[7] = {
		1.0L, 3.32192809488736234787L, 1.44269504088896340736L, 3.14159265358979323846L,
		0.30102999566398119521L, 0.69314718055994530942L, 0.0L
	};

	push(c, v[k]);
}

static void
d9special(c, rm)
	struct x86 *c;
	int rm;		/* D9 E0 + rm */
{
	real a, b, m;
	int e;

	switch (rm) {
	case 0x00:		/* fchs */
		set(c, 0, -get(c, 0));
		X->sw &= ~SW_C1;
		return;
	case 0x01:		/* fabs */
		a = get(c, 0);
		set(c, 0, a < 0 ? -a : a);
		X->sw &= ~SW_C1;
		return;
	case 0x04:		/* ftst */
		compare(c, get(c, 0), 0.0L, 0);
		return;
	case 0x05:		/* fxam */
		X->sw &= ~SW_CC;
		a = ST(0);
		if (a < 0 || (a == 0 && 1.0L / a < 0))
			X->sw |= SW_C1;
		if (empty(c, 0))
			X->sw |= SW_C3 | SW_C0;
		else if (isnanr(a))
			X->sw |= SW_C0;
		else if (isinfr(a))
			X->sw |= SW_C2 | SW_C0;
		else if (a == 0)
			X->sw |= SW_C3;
		else
			X->sw |= SW_C2;
		return;
	case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e:
		constant(c, rm - 8);
		return;
	case 0x10:		/* f2xm1 */
		set(c, 0, (real)pow(2.0, (double)get(c, 0)) - 1);
		return;
	case 0x11:		/* fyl2x */
		a = get(c, 0);
		b = get(c, 1);
		if (a == 0)
			X->sw |= SW_ZE;
		set(c, 1, b * (real)(log((double)a) / log(2.0)));
		pop(c);
		return;
	case 0x12:		/* fptan */
		set(c, 0, (real)tan((double)get(c, 0)));
		X->sw &= ~SW_C2;
		push(c, 1.0L);
		return;
	case 0x13:		/* fpatan */
		set(c, 1, (real)atan2((double)get(c, 1), (double)get(c, 0)));
		pop(c);
		return;
	case 0x14:		/* fxtract */
		a = get(c, 0);
		if (a == 0) {
			X->sw |= SW_ZE;
			set(c, 0, -1.0L / 0.0L);
			push(c, a);
			return;
		}
		if (isnanr(a) || isinfr(a)) {
			set(c, 0, isinfr(a) ? 1.0L / 0.0L : a);
			push(c, a);
			return;
		}
		m = split(a, &e);	/* the x87's significand is in [1, 2) */
		set(c, 0, (real)(e - 1));
		push(c, m * 2);
		return;
	case 0x15:		/* fprem1 */
	case 0x18:		/* fprem */
		a = get(c, 0);
		b = get(c, 1);
		X->sw &= ~SW_CC;
		if (b == 0 || isnanr(a) || isnanr(b) || isinfr(a)) {
			X->sw |= SW_IE;
			set(c, 0, indefinite);
			return;
		}
		{
			real q = a / b, r;
			u32 qi;

			q = rm == 0x18 ? trunc_r(q) : round_r(c, q);
			r = a - q * b;
			if (q < 0)
				q = -q;
			qi = (u32)fmod((double)q, 8.0);
			if (qi & 1) X->sw |= SW_C1;
			if (qi & 2) X->sw |= SW_C3;
			if (qi & 4) X->sw |= SW_C0;
			set(c, 0, r);
		}
		return;
	case 0x16:		/* fdecstp */
		X->top = (X->top - 1) & 7;
		X->sw &= ~SW_C1;
		return;
	case 0x17:		/* fincstp */
		X->top = (X->top + 1) & 7;
		X->sw &= ~SW_C1;
		return;
	case 0x19:		/* fyl2xp1 */
		set(c, 1, get(c, 1) * (real)(log(1.0 + (double)get(c, 0)) / log(2.0)));
		pop(c);
		return;
	case 0x1a:		/* fsqrt */
		a = get(c, 0);
		if (a < 0)
			X->sw |= SW_IE;
		set(c, 0, a < 0 ? indefinite : (real)sqrt((double)a));
		return;
	case 0x1b:		/* fsincos */
		a = get(c, 0);
		set(c, 0, (real)sin((double)a));
		X->sw &= ~SW_C2;
		push(c, (real)cos((double)a));
		return;
	case 0x1c:		/* frndint */
		set(c, 0, round_r(c, get(c, 0)));
		return;
	case 0x1d:		/* fscale */
		a = get(c, 0);
		b = trunc_r(get(c, 1));
		if (a != 0 && !isnanr(a) && !isinfr(a) && !isnanr(b))
			a = scale(a, b > 20000 ? 20000 : b < -20000 ? -20000 : (int)b);
		set(c, 0, a);
		return;
	case 0x1e:		/* fsin */
		set(c, 0, (real)sin((double)get(c, 0)));
		X->sw &= ~SW_C2;
		return;
	case 0x1f:		/* fcos */
		set(c, 0, (real)cos((double)get(c, 0)));
		X->sw &= ~SW_C2;
		return;
	}
	x86_exception(c, X_UD, 0);
}

void
x87_exec(c, op, mod, reg, rm, seg, off)
	struct x86 *c;
	int op, mod, reg, rm, seg;
	u32 off;
{
	real a, b;
	u32 lo, hi;
	int se, i;

	if (!c->x87)
		x87_init(c);
	/* the last instruction, for FSTENV */
	if (!(op == 0xd9 && mod != 3 && (reg == 4 || reg == 5 || reg == 6 || reg == 7)) &&
	    !(op == 0xdb && mod == 3 && rm >= 2 && rm <= 3)) {
		X->ip = c->ip0;
		X->ipsel = c->s[S_CS].sel;
		X->fop = ((op & 7) << 8) | (mod << 6) | (reg << 3) | rm;
		if (mod != 3) {
			X->op = off;
			X->opsel = c->s[seg].sel;
		}
	}
	if (mod == 3) {
		i = rm;
		switch (op) {
		case 0xd8:			/* st0 op st(i) */
			if (reg == 2 || reg == 3) {
				compare(c, get(c, 0), get(c, i), 0);
				if (reg == 3)
					pop(c);
				return;
			}
			set(c, 0, arith(c, reg, get(c, 0), get(c, i)));
			return;
		case 0xdc:			/* st(i) op st0, the sub and div reversed */
		case 0xde:			/* and pop */
			if (reg == 2 || reg == 3) {
				if (op == 0xde && !(reg == 3 && i == 1))
					break;
				compare(c, get(c, 0), get(c, i), 0);
				if (reg == 3)
					pop(c);
				if (op == 0xde)
					pop(c);	/* fcompp */
				return;
			}
			a = get(c, i);
			b = get(c, 0);
			set(c, i, arith(c, reg ^ (reg >= 4 ? 1 : 0), a, b));
			if (op == 0xde)
				pop(c);
			return;
		case 0xd9:
			switch (reg) {
			case 0:			/* fld st(i) */
				a = get(c, i);
				push(c, a);
				return;
			case 1:			/* fxch */
				a = get(c, 0);
				b = get(c, i);
				set(c, 0, b);
				set(c, i, a);
				X->sw &= ~SW_C1;
				return;
			case 2:
				if (rm == 0)	/* fnop */
					return;
				break;
			case 4: case 5: case 6: case 7:
				d9special(c, (reg - 4) * 8 + rm);
				return;
			}
			break;
		case 0xda:
			if (reg == 5 && i == 1) {	/* fucompp */
				compare(c, get(c, 0), get(c, 1), 1);
				pop(c);
				pop(c);
				return;
			}
			break;
		case 0xdb:
			if (reg == 4)
				switch (rm) {
				case 0: case 1: case 4:	/* feni, fdisi, fsetpm: the 287's, nothing */
					return;
				case 2:			/* fnclex */
					X->sw &= ~0x80ff;
					return;
				case 3:			/* fninit */
					x87_init(c);
					return;
				}
			break;
		case 0xdd:
			switch (reg) {
			case 0:			/* ffree */
				X->tag |= 3 << (2 * PHYS(i));
				return;
			case 2:			/* fst st(i) */
			case 3:			/* fstp st(i) */
				set(c, i, get(c, 0));
				if (reg == 3)
					pop(c);
				return;
			case 4:			/* fucom */
			case 5:			/* fucomp */
				compare(c, get(c, 0), get(c, i), 1);
				if (reg == 5)
					pop(c);
				return;
			}
			break;
		case 0xdf:
			if (reg == 4 && rm == 0) {	/* fnstsw ax */
				c->r[R_AX] = (c->r[R_AX] & ~0xffff) | (X->sw & ~0x3800) | (X->top << 11);
				return;
			}
			if (reg == 0) {			/* ffreep */
				X->tag |= 3 << (2 * PHYS(i));
				pop(c);
				return;
			}
			break;
		}
		x86_exception(c, X_UD, 0);
		return;
	}
	/* a memory operand */
	switch (op) {
	case 0xd8: case 0xdc: case 0xda: case 0xde:
		b = memreal(c, op, seg, off);
		if (reg == 2 || reg == 3) {
			compare(c, get(c, 0), b, 0);
			if (reg == 3)
				pop(c);
			return;
		}
		set(c, 0, arith(c, reg, get(c, 0), b));
		return;
	case 0xd9:
		switch (reg) {
		case 0:
			push(c, ld32(x86_rd(c, seg, off, 2)));
			return;
		case 2:
		case 3:
			x86_wr(c, seg, off, 2, st32(get(c, 0)));
			if (reg == 3)
				pop(c);
			return;
		case 4:
			ldenv(c, seg, off);
			return;
		case 5:
			X->cw = x86_rd(c, seg, off, 1);
			return;
		case 6:
			stenv(c, seg, off);
			X->cw |= 0x3f;	/* fnstenv masks the exceptions */
			return;
		case 7:
			x86_wr(c, seg, off, 1, X->cw);
			return;
		}
		break;
	case 0xdb:
		switch (reg) {
		case 0:
			push(c, ldint(x86_rd(c, seg, off, 2), 0, 32));
			return;
		case 2:
		case 3:
			stint(c, get(c, 0), 32, &lo, &hi);
			x86_wr(c, seg, off, 2, lo);
			if (reg == 3)
				pop(c);
			return;
		case 5:
			lo = x86_rd(c, seg, off, 2);
			hi = x86_rd(c, seg, off + 4, 2);
			push(c, ld80(lo, hi, (int)x86_rd(c, seg, off + 8, 1)));
			return;
		case 7:
			st80(get(c, 0), &lo, &hi, &se);
			x86_wr(c, seg, off, 2, lo);
			x86_wr(c, seg, off + 4, 2, hi);
			x86_wr(c, seg, off + 8, 1, se);
			pop(c);
			return;
		}
		break;
	case 0xdd:
		switch (reg) {
		case 0:
			push(c, ld64(x86_rd(c, seg, off, 2), x86_rd(c, seg, off + 4, 2)));
			return;
		case 2:
		case 3:
			st64(get(c, 0), &lo, &hi);
			x86_wr(c, seg, off, 2, lo);
			x86_wr(c, seg, off + 4, 2, hi);
			if (reg == 3)
				pop(c);
			return;
		case 4:			/* frstor */
			ldenv(c, seg, off);
			off += c->o32 ? 28 : 14;
			for (i = 0; i < 8; i++) {
				lo = x86_rd(c, seg, off + 10 * i, 2);
				hi = x86_rd(c, seg, off + 10 * i + 4, 2);
				X->st[PHYS(i)] = ld80(lo, hi, (int)x86_rd(c, seg, off + 10 * i + 8, 1));
			}
			return;
		case 6:			/* fnsave */
			stenv(c, seg, off);
			off += c->o32 ? 28 : 14;
			for (i = 0; i < 8; i++) {
				st80(X->st[PHYS(i)], &lo, &hi, &se);
				x86_wr(c, seg, off + 10 * i, 2, lo);
				x86_wr(c, seg, off + 10 * i + 4, 2, hi);
				x86_wr(c, seg, off + 10 * i + 8, 1, se);
			}
			x87_init(c);
			return;
		case 7:			/* fnstsw */
			x86_wr(c, seg, off, 1, (X->sw & ~0x3800) | (X->top << 11));
			return;
		}
		break;
	case 0xdf:
		switch (reg) {
		case 0:
			push(c, ldint(x86_rd(c, seg, off, 1), 0, 16));
			return;
		case 2:
		case 3:
			stint(c, get(c, 0), 16, &lo, &hi);
			x86_wr(c, seg, off, 1, lo);
			if (reg == 3)
				pop(c);
			return;
		case 4:			/* fbld: 18 packed decimal digits and a sign */
			a = 0;
			for (i = 8; i >= 0; i--) {
				int d = x86_rd(c, seg, off + i, 0);

				a = a * 100 + (d >> 4) * 10 + (d & 15);
			}
			push(c, (x86_rd(c, seg, off + 9, 0) & 0x80) ? -a : a);
			return;
		case 5:
			lo = x86_rd(c, seg, off, 2);
			hi = x86_rd(c, seg, off + 4, 2);
			push(c, ldint(lo, hi, 64));
			return;
		case 6:			/* fbstp */
			a = round_r(c, get(c, 0));
			b = a < 0 ? -a : a;
			if (isnanr(a) || b >= 1e18L) {
				X->sw |= SW_IE;
				for (i = 0; i < 7; i++)
					x86_wr(c, seg, off + i, 0, 0);
				x86_wr(c, seg, off + 7, 0, 0xc0);
				x86_wr(c, seg, off + 8, 0, 0xff);
				x86_wr(c, seg, off + 9, 0, 0xff);
			} else {
				for (i = 0; i < 9; i++) {
					real q = trunc_r(b / 100);
					int d = (int)(b - q * 100);

					x86_wr(c, seg, off + i, 0, (d / 10) << 4 | d % 10);
					b = q;
				}
				x86_wr(c, seg, off + 9, 0, a < 0 ? 0x80 : 0);
			}
			pop(c);
			return;
		case 7:
			stint(c, get(c, 0), 64, &lo, &hi);
			x86_wr(c, seg, off, 2, lo);
			x86_wr(c, seg, off + 4, 2, hi);
			pop(c);
			return;
		}
		break;
	}
	x86_exception(c, X_UD, 0);
}
