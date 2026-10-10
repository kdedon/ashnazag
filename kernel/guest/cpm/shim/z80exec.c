/* 8080 interpreter with selected Z80 extensions. Guest memory wraps at
 * 16 bits; words are little-endian. Lazy flags retain 8080 semantics,
 * including inverted subtraction auxiliary carry and parity for NEG.
 * Z80-only block operations use counter-based P/V. Unsupported operations
 * return X_UNIMP; this is not a complete Z80 implementation. */

#include "z80.h"

int	z80hookno;		/* hook left behind by X_HOOK		*/

/* The instruments.  Two counters, no branches on the hot path.  On the
 * target they cost two long increments; when that matters they go behind
 * a build flag, which is not yet. */
z32	z80ninsn;		/* instructions executed		*/
z32	z80nflag;		/* times a lazy record was materialised	*/

/* A delay loop is charged at Z80 T-states on a 4 MHz part. */
#define Z80TICK	40000L		/* T-states per 100 Hz tick		*/
#define T_DEC	4		/* DEC r				*/
#define T_JP	10		/* JP cc, taken or not			*/
#define T_JRY	12		/* JR cc, taken				*/
#define T_JRN	7		/* JR cc, not taken			*/

int	z80fast = 1;		/* delay loops in one step		*/
z32	z80nskip;		/* instructions they did not step	*/
int	(*z80wait)();		/* sleep n ticks; null does not sleep	*/
static z32 owed;		/* T-states not yet slept		*/

/* ------------------------------------------------------------------ */
/* memory: bytewise, little-endian, addresses wrap at 64 KB	       */

/* The guest byte at `a'.  The z16 is the wraparound: 0xFFFF + 1 is 0. */
#define MB(m, a)	((m)->m[(z16)(a)])

int z80rb(m, a)
struct z80 *m;
z16 a;
{
	return (MB(m, a) & 0xff);
}

int z80wb(m, a, v)
struct z80 *m;
z16 a;
int v;
{
	MB(m, a) = (char)v;
	return (0);
}

z16 z80rw(m, a)
struct z80 *m;
z16 a;
{
	return ((z16)((MB(m, a) & 0xff) | ((MB(m, a + 1) & 0xff) << 8)));
}

int z80ww(m, a, v)
struct z80 *m;
z16 a, v;
{
	MB(m, a) = (char)v;
	MB(m, a + 1) = (char)(v >> 8);
	return (0);
}

/* ------------------------------------------------------------------ */
/* registers							       */

/*
 * The 8080 r-field: 0 B, 1 C, 2 D, 3 E, 4 H, 5 L, 6 the byte at (HL),
 * 7 A.  Pairs are held as whole words with the 8080's high byte in the
 * high half, so B is the top of rp[0] and C the bottom -- which is what
 * makes the pair a native word for DAD, INX and PUSH, at no cost on the
 * target.
 *
 * Done arithmetically rather than by overlaying a char array on the pair
 * file, because the host and the Z8000 disagree about which end of a
 * word a byte lives at and this file must not.
 */
int z80getr(m, r)
struct z80 *m;
int r;
{
	if (r == R_A)
		return (m->a & 0xff);
	if (r == R_M)
		return (MB(m, m->rp[P_HL]) & 0xff);
	return ((r & 1) ? (m->rp[r >> 1] & 0xff)
			: ((m->rp[r >> 1] >> 8) & 0xff));
}

int z80setr(m, r, v)
struct z80 *m;
int r, v;
{
	register z16 w;

	if (r == R_A) {
		m->a = (z8)(v & 0xff);
		return (0);
	}
	if (r == R_M) {
		MB(m, m->rp[P_HL]) = (char)v;
		return (0);
	}
	w = m->rp[r >> 1];
	if (r & 1)
		w = (z16)((w & 0xff00) | (v & 0xff));
	else
		w = (z16)((w & 0x00ff) | ((z16)(v & 0xff) << 8));
	m->rp[r >> 1] = w;
	return (0);
}

/*
 * Past this point the six accessors ARE their expressions.  The
 * executor reaches them about twice a guest instruction and the call
 * frame costs more than the access does; the functions above stay
 * because the loader, the seam and the tests call them by name.
 *
 * Every argument below is evaluated more than once, so no call site in
 * this file may pass one with a side effect.
 *
 * The #undefs clear the renames a second build of this file is given.
 */
#undef z80rb
#undef z80wb
#undef z80rw
#undef z80ww
#undef z80getr
#undef z80setr
#define z80rb(m, a)		(MB(m, a) & 0xff)
#define z80wb(m, a, v)		(MB(m, a) = (char)(v))
#define z80rw(m, a)		((z16)((MB(m, a) & 0xff) \
				       | ((MB(m, (a) + 1) & 0xff) << 8)))
#define z80ww(m, a, v)		(MB(m, a) = (char)(v), \
				 MB(m, (a) + 1) = (char)((v) >> 8))
#define z80getr(m, r)		((r) == R_A ? (m)->a & 0xff \
				 : (r) == R_M ? MB(m, (m)->rp[P_HL]) & 0xff \
				 : (r) & 1 ? (m)->rp[(r) >> 1] & 0xff \
				 : ((m)->rp[(r) >> 1] >> 8) & 0xff)
#define z80setr(m, r, v)	((r) == R_A \
			? ((m)->a = (z8)(v), 0) \
			: (r) == R_M \
			? (MB(m, (m)->rp[P_HL]) = (char)(v), 0) \
			: (r) & 1 \
			? ((m)->rp[(r) >> 1] = (z16)(((m)->rp[(r) >> 1] \
				& 0xff00) | ((v) & 0xff)), 0) \
			: ((m)->rp[(r) >> 1] = (z16)(((m)->rp[(r) >> 1] \
				& 0x00ff) | ((z16)((v) & 0xff) << 8)), 0))

/* ------------------------------------------------------------------ */
/* flags								*/

/* PF is EVEN parity of the byte: set when the number of set bits is
 * even.  A table, because counting the bits costs eight turns of a loop
 * on a path every arithmetic instruction reaches. */
static z8 partab[256] = {
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,	/* 00 */
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,	/* 10 */
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,	/* 20 */
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,	/* 30 */
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,	/* 40 */
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,	/* 50 */
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,	/* 60 */
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,	/* 70 */
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,	/* 80 */
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,	/* 90 */
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,	/* A0 */
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,	/* B0 */
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,	/* C0 */
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,	/* D0 */
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,	/* E0 */
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1	/* F0 */
};

static int par8(v)
int v;
{
	return ((int)partab[v & 0xff]);
}

/*
 * Materialise the pending lazy record into m->f and return F.
 * Idempotent: after it runs, m->lz is LZ_NONE and m->f is complete.
 *
 * Every record here is byte wide -- the 8080's only 16-bit flag-setting
 * instruction is DAD, which touches CY alone and is therefore computed
 * eagerly at its case.  That is why this function has no width argument
 * and i86flags() needed one, and it is a real simplification rather than
 * an omission.
 *
 * The casts to z32 make the carry comparisons unsigned in both worlds:
 * on the target a z16 promotes to unsigned int, on the host to a 32-bit
 * signed int, and the two disagree about `<' unless the comparison is
 * made explicitly wide.  i86exec.c learned this the same way.
 */
z8 z80flags(m)
struct z80 *m;
{
	register int f;
	register int a, b, r;

	if (m->lz == LZ_NONE)
		return (m->f);
	z80nflag++;
	a = m->la & 0xff;
	b = m->lb & 0xff;
	r = m->lr & 0xff;
	f = F_ONE;
	if (r == 0)
		f |= F_ZE;
	if (r & 0x80)
		f |= F_SI;
	if (par8(r))
		f |= F_PA;
	switch (m->lz) {
	case LZ_ADD:
		/* r = a + b + c.  The carry out of the top is visible as
		 * the sum having failed to grow; with a carry in, a sum
		 * merely equal to a has already wrapped. */
		if (m->lc ? ((z32)r <= (z32)a) : ((z32)r < (z32)a))
			f |= F_CY;
		if ((a ^ b ^ r) & 0x10)
			f |= F_AC;
		break;
	case LZ_SUB:
		if (m->lc ? ((z32)a <= (z32)b) : ((z32)a < (z32)b))
			f |= F_CY;
		/* The 8080 carries the half-BORROW inverted: AC is set
		 * when there was NO borrow out of bit 3.  That is not a
		 * transcription slip -- it is what makes DAA work after
		 * SUB on this part, and it is the one flag rule here
		 * that reads backwards from the addition case. */
		if (!((a ^ b ^ r) & 0x10))
			f |= F_AC;
		break;
	case LZ_AND:
		/* ANA is the 8080's odd one: CY is cleared, and AC comes
		 * from bit 3 of (A | src) rather than from the result.
		 * A Z80 sets H unconditionally here; see the divergence
		 * note at the head of this file. */
		if ((a | b) & 0x08)
			f |= F_AC;
		break;
	case LZ_LOG:
		/* ORA and XRA clear both CY and AC. */
		break;
	case LZ_INR:
		/* CY is PRESERVED by INR and DCR -- the one place the
		 * 8080's flag rules are not uniform, and the reason
		 * these are separate classes rather than ADD with b = 1. */
		f |= (int)(m->f & F_CY);
		if ((a ^ b ^ r) & 0x10)
			f |= F_AC;
		break;
	case LZ_DCR:
		f |= (int)(m->f & F_CY);
		if (!((a ^ b ^ r) & 0x10))
			f |= F_AC;
		break;

	/* ---- the prefix-group classes.  Z80 rules, because these
	 * encodings have no 8080 form to be an 8080 about. */
	case LZ_ROT:
		/* The whole CB rotate/shift group: CY is the bit that
		 * left the byte, H and N are cleared, and P/V is
		 * PARITY, the one place the CB group agrees with the
		 * 8080's own RLC/RRC. */
		if (m->lc)
			f |= F_CY;
		break;
	case LZ_BIT:
		/* BIT b,r sets H, clears N, PRESERVES CY, and defines
		 * P/V as a copy of Z.  `r' here is the MASKED byte --
		 * v & (1 << b) -- and that one choice makes the common
		 * prologue above compute all three of S, Z and P/V
		 * correctly with no special case: the masked byte is
		 * zero exactly when the bit is clear, has bit 7 set
		 * exactly when the Z80 sets S (b = 7 and the bit set),
		 * and has even parity exactly when it is zero, which IS
		 * P/V = Z for a value with at most one bit in it. */
		f |= (int)(m->f & F_CY);
		f |= F_AC;
		break;
	case LZ_LDBLK:
		/* LDI/LDD and their repeating forms: S, Z and CY are
		 * untouched by the transfer, H and N are cleared, and
		 * P/V says whether the counter is still non-zero.  The
		 * preserved three are read back out of m->f, which
		 * lazy() has already made current. */
		f = (int)(m->f & (F_SI | F_ZE | F_CY)) | F_ONE;
		if (m->lc)
			f |= F_PA;
		break;
	case LZ_CPBLK:
		/* CPI/CPD: S, Z and H come from A - (HL) as they do for
		 * any subtract, CY is PRESERVED, and P/V is the counter
		 * again rather than parity -- so the prologue's parity
		 * is overwritten here and not merely added to.
		 *
		 * AC follows THIS FILE's 8080 convention (set when there
		 * was no half borrow), not the Z80's, so that the bit
		 * means one thing throughout: a DAA reached after a
		 * block compare would otherwise read a half-carry with
		 * the opposite sense from every other subtract here. */
		f &= ~F_PA;
		if (m->lc)
			f |= F_PA;
		f |= (int)(m->f & F_CY);
		if (!((a ^ b ^ r) & 0x10))
			f |= F_AC;
		break;
	}
	m->f = (z8)(f & ~F_Z80X);
	m->lz = LZ_NONE;
	return (m->f);
}

/* Record a lazy result.  `c' is the carry in for ADC/SBB and 0 else. */
static lazy(m, cls, a, b, r, c)
struct z80 *m;
int cls, a, b, r, c;
{
	if (cls == LZ_INR || cls == LZ_DCR) {
		/* CY must survive: fold first, unless a pending INR or
		 * DCR already left it in m->f. */
		if (m->lz != LZ_INR && m->lz != LZ_DCR)
			z80flags(m);
	} else if (cls == LZ_BIT || cls == LZ_LDBLK || cls == LZ_CPBLK) {
		/* The same rule -- all three read bits they do not
		 * write -- with the one exception that is the whole of
		 * the lazy scheme's value in a block move: if the
		 * PENDING record is already this class, the bits this
		 * class preserves are still exactly the bits in m->f,
		 * because preserving them is what the pending record
		 * does.  Folding again would materialise once per
		 * iteration and buy nothing, so an LDIR over 4 KB costs
		 * ONE materialisation and not 4,096. */
		if (m->lz != (z8)cls)
			z80flags(m);
	}
	m->lz = (z8)cls;
	m->la = (z16)(a & 0xff);
	m->lb = (z16)(b & 0xff);
	m->lr = (z16)(r & 0xff);
	m->lc = (z8)c;
	return (0);
}

/*
 * z80cond(z80flags(m), cc) without materialising: the one flag tested
 * comes straight from the record by z80flags()' rule for it.  CY of a
 * class that preserves it is already in m->f, as lazy() folded first.
 */
int z80lcond(m, cc)
register struct z80 *m;
register int cc;
{
	register int t;

	if (m->lz == LZ_NONE)
		return (z80cond((int)m->f, cc));
	if (m->lz == LZ_LDBLK)
		return (z80cond((int)z80flags(m), cc));
	switch (cc >> 1) {
	case 0:						/* Z	*/
		t = m->lr == 0;
		break;
	case 1:						/* CY	*/
		switch (m->lz) {
		case LZ_ADD:
			t = m->lc ? m->lr <= m->la : m->lr < m->la;
			break;
		case LZ_SUB:
			t = m->lc ? m->la <= m->lb : m->la < m->lb;
			break;
		case LZ_AND:
		case LZ_LOG:
			t = 0;
			break;
		case LZ_ROT:
			t = m->lc;
			break;
		default:
			t = m->f & F_CY;
			break;
		}
		break;
	case 2:						/* PE	*/
		t = m->lz == LZ_CPBLK ? m->lc : par8(m->lr);
		break;
	default:					/* M	*/
		t = m->lr & 0x80;
		break;
	}
	return ((cc & 1) ? t != 0 : t == 0);
}

/* ------------------------------------------------------------------ */
/* the stack							       */

static push(m, v)
struct z80 *m;
z16 v;
{
	m->rp[P_SP] = (z16)(m->rp[P_SP] - 2);
	z80ww(m, m->rp[P_SP], v);
	return (0);
}

static z16 pop(m)
struct z80 *m;
{
	register z16 v;

	v = z80rw(m, m->rp[P_SP]);
	m->rp[P_SP] = (z16)(m->rp[P_SP] + 2);
	return (v);
}

/* ------------------------------------------------------------------ */
/* the ALU eight						       */

/*
 * Returns the result; records the lazy flags; the caller decides whether
 * to store it (CMP does not).  `b' is the source byte.
 */
static int alu(m, aop, b)
struct z80 *m;
int aop, b;
{
	register int a, r, c;

	a = m->a & 0xff;
	b &= 0xff;
	switch (aop) {
	case 0:					/* ADD			*/
		r = (a + b) & 0xff;
		lazy(m, LZ_ADD, a, b, r, 0);
		break;
	case 1:					/* ADC			*/
		c = (z80flags(m) & F_CY) != 0;
		r = (a + b + c) & 0xff;
		lazy(m, LZ_ADD, a, b, r, c);
		break;
	case 2:					/* SUB			*/
		r = (a - b) & 0xff;
		lazy(m, LZ_SUB, a, b, r, 0);
		break;
	case 3:					/* SBB			*/
		c = (z80flags(m) & F_CY) != 0;
		r = (a - b - c) & 0xff;
		lazy(m, LZ_SUB, a, b, r, c);
		break;
	case 4:					/* ANA			*/
		r = a & b;
		lazy(m, LZ_AND, a, b, r, 0);
		break;
	case 5:					/* XRA			*/
		r = a ^ b;
		lazy(m, LZ_LOG, a, b, r, 0);
		break;
	case 6:					/* ORA			*/
		r = a | b;
		lazy(m, LZ_LOG, a, b, r, 0);
		break;
	default:				/* CMP			*/
		r = (a - b) & 0xff;
		lazy(m, LZ_SUB, a, b, r, 0);
		break;
	}
	return (r);
}

/* 8080 decimal adjust uses accumulator, auxiliary carry, and carry.
 * Compute flags explicitly so host and target follow the same rules. */
static int daa(m)
struct z80 *m;
{
	register int a, add, cy, f;

	f = z80flags(m);
	a = m->a & 0xff;
	add = 0;
	cy = (f & F_CY) != 0;
	if ((a & 0x0f) > 9 || (f & F_AC))
		add |= 0x06;
	if (a > 0x99 || cy) {
		add |= 0x60;
		cy = 1;
	}
	/* AC after the correction is the half-carry of the ADDITION that
	 * the correction performs, which is why it is computed from the
	 * two operands and not carried over from the flag that selected
	 * the correction. */
	f = F_ONE;
	if (((a & 0x0f) + (add & 0x0f)) > 0x0f)
		f |= F_AC;
	if (cy)
		f |= F_CY;
	a = (a + add) & 0xff;
	if (a == 0)
		f |= F_ZE;
	if (a & 0x80)
		f |= F_SI;
	if (par8(a))
		f |= F_PA;
	m->a = (z8)a;
	m->f = (z8)(f & ~F_Z80X);
	m->lz = LZ_NONE;
	return (0);
}

/*
 * The four rotates.  Only CY is affected; S, Z, AC and P survive, which
 * is why the pending lazy record is folded first and then edited rather
 * than replaced.  A naming trap waits for the assembly path: the 8080's
 * RAL/RAR rotate THROUGH carry and its RLC/RRC do not, and the Z8000
 * spells the same two the other way round.
 */
static int rot(m, which)
struct z80 *m;
int which;
{
	register int a, cy, f;

	f = z80flags(m);
	a = m->a & 0xff;
	cy = (f & F_CY) != 0;
	switch (which) {
	case 0:					/* RLC			*/
		cy = (a >> 7) & 1;
		a = ((a << 1) | cy) & 0xff;
		break;
	case 1:					/* RRC			*/
		cy = a & 1;
		a = ((a >> 1) | (cy << 7)) & 0xff;
		break;
	case 2:					/* RAL			*/
		f = (a >> 7) & 1;
		a = ((a << 1) | cy) & 0xff;
		cy = f;
		break;
	default:				/* RAR			*/
		f = a & 1;
		a = ((a >> 1) | (cy << 7)) & 0xff;
		cy = f;
		break;
	}
	m->a = (z8)a;
	f = (m->f & ~(F_CY | F_Z80X)) | F_ONE;
	if (cy)
		f |= F_CY;
	m->f = (z8)f;
	return (0);
}

/* ------------------------------------------------------------------ */
/* the CB group: rotates and shifts, and the bit three		       */

/* CB fields select group, register, and rotate operation or bit number.
 * RES/SET preserve flags; BIT preserves carry and sets auxiliary carry. */
static int cbop(m, in)
struct z80 *m;
struct z80in *in;
{
	register int v, r, cy;
	int b, c;

	v = z80getr(m, in->y) & 0xff;
	b = (int)(in->imm & 7);
	if (in->x == 0) {
		cy = 0;
		switch (b) {
		case 0:				/* RLC			*/
			cy = (v >> 7) & 1;
			r = ((v << 1) | cy) & 0xff;
			break;
		case 1:				/* RRC			*/
			cy = v & 1;
			r = ((v >> 1) | (cy << 7)) & 0xff;
			break;
		case 2:				/* RL: through carry	*/
			c = (z80flags(m) & F_CY) != 0;
			cy = (v >> 7) & 1;
			r = ((v << 1) | c) & 0xff;
			break;
		case 3:				/* RR: through carry	*/
			c = (z80flags(m) & F_CY) != 0;
			cy = v & 1;
			r = ((v >> 1) | (c << 7)) & 0xff;
			break;
		case 4:				/* SLA			*/
			cy = (v >> 7) & 1;
			r = (v << 1) & 0xff;
			break;
		case 5:				/* SRA: sign kept	*/
			cy = v & 1;
			r = ((v >> 1) | (v & 0x80)) & 0xff;
			break;
		case 6:				/* SLL: undocumented	*/
			cy = (v >> 7) & 1;
			r = ((v << 1) | 1) & 0xff;
			break;
		default:			/* SRL			*/
			cy = v & 1;
			r = (v >> 1) & 0xff;
			break;
		}
		z80setr(m, in->y, r);
		lazy(m, LZ_ROT, v, 0, r, cy);
		return (0);
	}
	if (in->x == 1) {			/* BIT b,r		*/
		lazy(m, LZ_BIT, v, b, v & (1 << b), 0);
		return (0);
	}
	if (in->x == 2)				/* RES b,r		*/
		r = v & ~(1 << b);
	else					/* SET b,r		*/
		r = v | (1 << b);
	z80setr(m, in->y, r & 0xff);
	return (0);
}

/* ------------------------------------------------------------------ */
/* the ED group: the block operations and the 16-bit arithmetic	       */

/* Execute one block iteration per step, backing PC up by two when it
 * repeats. The outer instruction budget and debugger therefore remain
 * effective even for BC=0 (65536 iterations). */
static int edop(m, in)
struct z80 *m;
struct z80in *in;
{
	register int sub, v, r;
	int a, up, rep;
	z16 hl, de, bc, ss;
	z32 w;

	sub = (int)in->sub;

	/* ---- A0/A8/B0/B8: LDI, LDD, LDIR, LDDR ---- */
	if (sub == 0xa0 || sub == 0xa8 || sub == 0xb0 || sub == 0xb8) {
		up = (sub & 8) == 0;		/* A0/B0 up, A8/B8 down	*/
		rep = (sub & 0x10) != 0;	/* B0/B8 repeat		*/
		hl = m->rp[P_HL];
		de = m->rp[P_DE];
		z80wb(m, de, z80rb(m, hl));
		m->rp[P_HL] = (z16)(up ? hl + 1 : hl - 1);
		m->rp[P_DE] = (z16)(up ? de + 1 : de - 1);
		bc = (z16)(m->rp[P_BC] - 1);
		m->rp[P_BC] = bc;
		lazy(m, LZ_LDBLK, 0, 0, 0, bc != 0);
		if (rep && bc != 0)
			m->pc = (z16)(m->pc - 2);
		return (X_OK);
	}

	/* ---- A1/A9/B1/B9: CPI, CPD, CPIR, CPDR ---- */
	if (sub == 0xa1 || sub == 0xa9 || sub == 0xb1 || sub == 0xb9) {
		up = (sub & 8) == 0;
		rep = (sub & 0x10) != 0;
		hl = m->rp[P_HL];
		v = z80rb(m, hl);
		a = m->a & 0xff;
		r = (a - v) & 0xff;
		m->rp[P_HL] = (z16)(up ? hl + 1 : hl - 1);
		bc = (z16)(m->rp[P_BC] - 1);
		m->rp[P_BC] = bc;
		/* A is NOT written: this is a compare.  The `carry in'
		 * slot carries BC != 0, which is what P/V becomes. */
		lazy(m, LZ_CPBLK, a, v, r, bc != 0);
		/* The repeat stops on EITHER exhaustion or a match --
		 * the second is the whole point of CPIR and the half a
		 * loop written from LDIR would leave out. */
		if (rep && bc != 0 && r != 0)
			m->pc = (z16)(m->pc - 2);
		return (X_OK);
	}

	if (sub < 0x40 || sub > 0x7f)
		return (X_UNIMP);

	switch (sub & 15) {

	/* ---- x2/xA: SBC HL,ss and ADC HL,ss ---- */
	case 0x02: case 0x0a:
		z80flags(m);			/* the carry in, and the */
						/* fold the edit below needs */
		hl = m->rp[P_HL];
		ss = m->rp[(sub >> 4) & 3];
		v = (m->f & F_CY) != 0;
		if (sub & 8)
			w = (z32)hl + (z32)ss + (z32)v;
		else
			w = (z32)hl - (z32)ss - (z32)v;
		r = (int)(w & 0xffffL);
		/* Z80 rules throughout: this encoding has no 8080 form
		 * to be an 8080 about, so P/V is OVERFLOW -- the sign of
		 * the result disagreeing with the sign the operands
		 * demanded -- and not parity.  AC keeps THIS FILE's
		 * convention (the half carry of the addition, the
		 * inverted half borrow of the subtraction), taken at bit
		 * 12 because that is where a 16-bit BCD digit boundary
		 * falls. */
		a = F_ONE;
		if (r == 0)
			a |= F_ZE;
		if (r & 0x8000)
			a |= F_SI;
		if (w & 0x10000L)
			a |= F_CY;
		if (sub & 8) {
			if ((hl ^ (z16)r) & (ss ^ (z16)r) & 0x8000)
				a |= F_PA;
			if ((hl ^ ss ^ (z16)r) & 0x1000)
				a |= F_AC;
		} else {
			if ((hl ^ ss) & (hl ^ (z16)r) & 0x8000)
				a |= F_PA;
			if (!((hl ^ ss ^ (z16)r) & 0x1000))
				a |= F_AC;
		}
		m->rp[P_HL] = (z16)r;
		m->f = (z8)(a & ~F_Z80X);
		m->lz = LZ_NONE;
		return (X_OK);

	/* ---- x3/xB: LD (nn),dd and LD dd,(nn) ---- */
	case 0x03:
		z80ww(m, in->imm, m->rp[in->x]);
		return (X_OK);
	case 0x0b:
		m->rp[in->x] = z80rw(m, in->imm);
		return (X_OK);

	/* ---- x4/xC: NEG, in all eight of its encodings ---- */
	case 0x04: case 0x0c:
		v = m->a & 0xff;
		r = (0 - v) & 0xff;
		m->a = (z8)r;
		/* A subtraction from zero, and recorded as one: the
		 * lazy record already knows how to make CY (set unless
		 * A was zero), AC and the rest of it.
		 *
		 * Which means P/V here is parity and not the Z80's
		 * overflow -- NEG shares the byte subtract record with
		 * SUB, SBB and CMP.  The 16-bit ADC/SBC above are
		 * overflow, sharing nothing with an 8080 form. */
		lazy(m, LZ_SUB, 0, v, r, 0);
		return (X_OK);
	}
	return (X_UNIMP);
}

/*
 * A DCR r at pc0 that JNZ pc0 or JR NZ,pc0 follows is a delay loop.  With
 * r the count left after the DCR just run, finish it in one step, leaving
 * the state and counts its 2r+1 remaining instructions would, then sleep
 * the time the whole loop takes on a real Z80.
 */
int z80delay(m, x, pc0, r)
struct z80 *m;
int x, r;
z16 pc0;
{
	register z32 n, t;
	int len;

	if ((MB(m, pc0 + 1) & 0xff) == 0xc2 && z80rw(m, pc0 + 2) == pc0) {
		len = 3;
		t = (z32)(r + 1) * (T_DEC + T_JP);
	} else if ((MB(m, pc0 + 1) & 0xff) == 0x20
		&& (MB(m, pc0 + 2) & 0xff) == 0xfd) {
		len = 2;
		t = (z32)(r + 1) * T_DEC + (z32)r * T_JRY + T_JRN;
	} else
		return (0);
	/* Neither the jumps nor a DCR after a DCR materialise. */
	z80setr(m, x, 0);
	lazy(m, LZ_DCR, 1, 1, 0, 0);
	z80ninsn += 2 * r + 1;
	z80nskip += 2 * r + 1;
	m->pc = (z16)(pc0 + 1 + len);
	if (!z80wait)
		return (0);
	owed += t;
	if (owed >= Z80TICK) {
		n = owed / Z80TICK;
		owed -= n * Z80TICK;
		/* A wait of k ticks can end just after k - 1. */
		(*z80wait)((int)n + 1);
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* one instruction						       */

/*
 * z80step -- decode and execute the instruction at PC.
 *
 * PC is advanced past the instruction BEFORE it executes, so that a
 * relative branch target (computed at decode time from the same base)
 * and a pushed return address agree with the hardware without either
 * having to know the instruction's length.  A refusal leaves PC where it
 * was, so the caller can report the address that could not run -- which
 * is what makes X_UNIMP a worklist entry rather than a mystery.
 */
int z80step(m, in)
struct z80 *m;
struct z80in *in;
{
	z16 pc0;

	pc0 = m->pc;
	z80dec(m->m, m->pc, in);
	if (in->op == Z_BAD)
		return (X_BAD);
	z80ninsn++;
	m->pc = (z16)(m->pc + in->len);
	return (z80exec(m, in, pc0));
}

/* Execute *in, decoded at pc0, with PC already past it. */
int z80exec(m, in, pc0)
struct z80 *m;
struct z80in *in;
z16 pc0;
{
	register int v, r;
	z16 a;

	switch (in->op) {

	case Z_NOP:
		break;

	case Z_LDRR:
		/* MOV M,M does not exist: 0x76 is HLT, and z80dec.c
		 * takes it out of this range before we see it. */
		v = z80getr(m, in->y);
		z80setr(m, in->x, v);
		break;
	case Z_LDRI:
		z80setr(m, in->x, in->imm & 0xff);
		break;

	case Z_ALU:
		v = (in->fl & ZF_IMM) ? (int)(in->imm & 0xff)
				      : z80getr(m, in->y);
		r = alu(m, in->x, v);
		if (in->x != 7)			/* CMP stores nothing	*/
			m->a = (z8)r;
		break;

	case Z_INR:
		v = z80getr(m, in->x);
		r = (v + 1) & 0xff;
		lazy(m, LZ_INR, v, 1, r, 0);
		z80setr(m, in->x, r);
		break;
	case Z_DCR:
		v = z80getr(m, in->x);
		r = (v - 1) & 0xff;
		lazy(m, LZ_DCR, v, 1, r, 0);
		z80setr(m, in->x, r);
		if (r != 0 && in->x != R_M && z80fast)
			z80delay(m, in->x, pc0, r);
		break;

	case Z_LXI:
		m->rp[in->x] = in->imm;
		break;
	case Z_INX:
		m->rp[in->x] = (z16)(m->rp[in->x] + 1);
		break;
	case Z_DCX:
		m->rp[in->x] = (z16)(m->rp[in->x] - 1);
		break;
	case Z_DAD:
		/* The only 16-bit flag-setting instruction on the part,
		 * and it sets CY alone.  Computed eagerly, which is why
		 * z80flags() has no width and needs none: the pending
		 * record is folded first so that the S/Z/AC/P it owns
		 * are not lost, then CY is edited in place. */
		z80flags(m);
		{
			z32 s;

			s = (z32)m->rp[P_HL] + (z32)m->rp[in->x];
			m->rp[P_HL] = (z16)(s & 0xffffL);
			m->f = (z8)((m->f & ~(F_CY | F_Z80X)) | F_ONE);
			if (s & 0x10000L)
				m->f |= F_CY;
		}
		break;

	case Z_LDAX:
		m->a = (z8)z80rb(m, m->rp[in->x]);
		break;
	case Z_STAX:
		z80wb(m, m->rp[in->x], m->a & 0xff);
		break;
	case Z_LDA:
		m->a = (z8)z80rb(m, in->imm);
		break;
	case Z_STA:
		z80wb(m, in->imm, m->a & 0xff);
		break;
	case Z_LHLD:
		m->rp[P_HL] = z80rw(m, in->imm);
		break;
	case Z_SHLD:
		z80ww(m, in->imm, m->rp[P_HL]);
		break;

	case Z_ROT:
		rot(m, in->x);
		break;
	case Z_DAA:
		daa(m);
		break;
	case Z_CMA:
		m->a = (z8)(~m->a & 0xff);	/* CMA affects no flags	*/
		break;
	case Z_STC:
		z80flags(m);
		m->f = (z8)((m->f | F_CY | F_ONE) & ~F_Z80X);
		break;
	case Z_CMC:
		z80flags(m);
		m->f = (z8)(((m->f ^ F_CY) | F_ONE) & ~F_Z80X);
		break;

	case Z_JMP:
		m->pc = in->imm;
		break;
	case Z_JCC:
		if (z80lcond(m, (int)in->x))
			m->pc = in->imm;
		break;
	case Z_CALL:
		push(m, m->pc);
		m->pc = in->imm;
		break;
	case Z_CCC:
		if (z80lcond(m, (int)in->x)) {
			push(m, m->pc);
			m->pc = in->imm;
		}
		break;
	case Z_RET:
		m->pc = pop(m);
		break;
	case Z_RCC:
		if (z80lcond(m, (int)in->x))
			m->pc = pop(m);
		break;
	case Z_RST:
		push(m, m->pc);
		m->pc = (z16)(in->x * 8);
		break;

	case Z_PCHL:
		m->pc = m->rp[P_HL];
		break;
	case Z_SPHL:
		m->rp[P_SP] = m->rp[P_HL];
		break;
	case Z_XCHG:
		a = m->rp[P_HL];
		m->rp[P_HL] = m->rp[P_DE];
		m->rp[P_DE] = a;
		break;
	case Z_XTHL:
		a = z80rw(m, m->rp[P_SP]);
		z80ww(m, m->rp[P_SP], m->rp[P_HL]);
		m->rp[P_HL] = a;
		break;

	case Z_PUSH:
		/* rp = 3 is PSW here, not SP: the push/pop encodings
		 * substitute the accumulator and flags for the stack
		 * pointer, which is the one place the rp field does not
		 * mean what it means everywhere else. */
		if (in->x == 3)
			push(m, (z16)(((z16)(m->a & 0xff) << 8)
				    | (z80flags(m) & 0xff)));
		else
			push(m, m->rp[in->x]);
		break;
	case Z_POP:
		a = pop(m);
		if (in->x == 3) {
			m->a = (z8)((a >> 8) & 0xff);
			/* Bit 1 reads back as one and bits 5 and 3 as
			 * zero on an 8080, whatever was pushed.  Period
			 * code does compare a popped flag byte, so the
			 * mask is applied rather than trusted. */
			m->f = (z8)(((a & 0xff) | F_ONE) & ~F_Z80X);
			m->lz = LZ_NONE;
		} else
			m->rp[in->x] = a;
		break;

	case Z_EI:
		m->iff = 1;
		break;
	case Z_DI:
		m->iff = 0;
		break;

	/* ---- the four Z80 base-map opcodes, none of which costs a
	 * register or a table. */
	case Z_JR:
		if (in->x == 4 || z80lcond(m, (int)in->x))
			m->pc = in->imm;
		break;
	case Z_DJNZ:
		/* B, and only B: the Z80 spells the counter into the
		 * opcode, so there is no register field to read. */
		v = (z80getr(m, R_B) - 1) & 0xff;
		z80setr(m, R_B, v);
		if (v != 0)
			m->pc = in->imm;
		break;
	case Z_EXAF:
		z80flags(m);			/* AF' must get real flags */
		{
			z8 t;

			t = m->a; m->a = m->aa; m->aa = t;
			t = m->f; m->f = m->af; m->af = t;
		}
		m->f = (z8)((m->f | F_ONE) & ~F_Z80X);
		break;
	case Z_CB:
		cbop(m, in);
		break;

	case Z_EXX:
		{
			int i;
			z16 t;

			for (i = 0; i < 3; i++) {
				t = m->rp[i];
				m->rp[i] = m->arp[i];
				m->arp[i] = t;
			}
		}
		break;

	case Z_HOOK:
		/* The BDOS/BIOS escape.  PC is already past the three
		 * bytes, so a hook that behaves like a subroutine simply
		 * returns and the guest resumes after it. */
		z80hookno = (int)in->x;
		return (X_HOOK);

	case Z_HLT:
		m->halt = 1;
		m->pc = pc0;
		return (X_HALT);

	/* ---- decoded but not executed; decoding them buys a refusal
	 * that names the instruction and its address instead of a core
	 * that quietly does something else. */
	case Z_ED:
		/* The implemented half returns X_OK; the rest is a
		 * refusal that names itself, and PC goes back so that
		 * the caller reports the address that could not run. */
		if (edop(m, in) == X_OK)
			break;
		m->pc = pc0;
		return (X_UNIMP);

	case Z_IX: case Z_IXCB:			/* the IX/IY overlay	*/
	case Z_IN: case Z_OUT:			/* no port hardware	*/
	default:
		m->pc = pc0;
		return (X_UNIMP);
	}
	return (X_OK);
}
