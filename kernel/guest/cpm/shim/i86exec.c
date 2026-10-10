/* 8086 interpreter: bytewise guest memory, lazy ALU flags, and decoded
 * instruction dispatch. Unsupported operations return X_UNIMP.
 * Segment writes resolve assigned paragraphs or bounded interior windows;
 * a failed reference is reported as X_WINDOW after dispatch. */

#include "i86.h"

int	i86intno;		/* vector left behind by X_INT		*/
i16	i86segbad;		/* paragraph that caused X_SEGESC	*/

/*
 * The assigned-paragraph set.  The shim hands out one 64 KB host
 * segment per guest group and records the pair here.  The check sits on
 * the WRITE to a segment register, not on the arithmetic that produced
 * the value: WordStar stashes an absolute segment in a variable and
 * reloads ES from it much later.
 */
i16	i86spar[I86NSEG];	/* guest paragraph			*/
char	*i86sbase[I86NSEG];	/* the host segment we gave it		*/
int	i86nseg;
i32	i86nsegslow;		/* slow-path resolutions		*/
i32	i86nsegbad;		/* ... and the writes it could not cover	*/

/* Hook for handing out a segment on demand.  Null by default, so an
 * unresolvable paragraph is a refusal -- the only answer a build with no
 * spare segment can give. */
char	*(*i86segnew)();

/* Two counters, no branches on the hot path.  They cost a long
 * increment each on the target. */
i32	i86ninsn;		/* instructions executed		*/
i32	i86nflag;		/* times a lazy record was materialised	*/

/* A delay loop is charged at 8086 clocks on a 4.77 MHz part. */
#define I86TICK	47700L		/* clocks per 100 Hz tick		*/
#define C_DECRM	3		/* DEC r, the mod r/m form		*/
#define C_DECR	2		/* DEC r16, the one-byte form		*/
#define C_JCCY	16		/* Jcc, taken				*/
#define C_JCCN	4		/* Jcc, not taken			*/

int	i86fast = 1;		/* delay loops in one step		*/
i32	i86nskip;		/* instructions they did not step	*/
int	(*i86wait)();		/* sleep n ticks; null does not sleep	*/
i16	i86nrun;		/* steps i86run() has left		*/
static i32 owed;		/* clocks not yet slept			*/

char *i86resolve(par)
i16 par;
{
	register int i;

	for (i = 0; i < i86nseg; i++)
		if (i86spar[i] == par)
			return (i86sbase[i]);
	return ((char *)0);
}

/* ------------------------------------------------------------------ */
/* registers							       */

/* Byte registers are AL CL DL BL AH CH DH BH for reg = 0..7: the low
 * halves first, then the high ones.  Done arithmetically rather than by
 * overlaying a char array on the word file, because the host and the
 * Z8000 disagree about which end of a word a byte lives at and this
 * file must not.
 *
 * These accessors and the ones below are macros: they run several times
 * a guest instruction and a call frame costs more than the access.
 * Every argument may be evaluated more than once, so none may have a
 * side effect. */
#define getb(m, n)	((int)((n) & 4 ? ((m)->r[(n) & 3] >> 8) & 0xff \
				       : (m)->r[(n) & 3] & 0xff))
#define setb(m, n, v)	((m)->r[(n) & 3] = (i16)((n) & 4 \
			? ((m)->r[(n) & 3] & 0x00ff) | ((i16)((v) & 0xff) << 8) \
			: ((m)->r[(n) & 3] & 0xff00) | ((v) & 0xff)))

/* ------------------------------------------------------------------ */
/* memory: bytewise, little-endian, offsets wrap inside the segment    */

/* Guest addresses use a segment base plus a 16-bit bias and offset.
 * Detect bias+offset wrap before accessing memory. Zero-bias segments
 * retain normal guest offset wraparound. */
static int mrb(m, s, off)
struct i86 *m;
int s;
i16 off;
{
	register i16 t;

	s &= 3;
	t = (i16)(m->so[s] + off);
	if (t < m->so[s]) {
		m->fault = 1;
		m->fseg = (i8)s;
		m->foff = off;
		return (0);
	}
	return (m->sb[s][t] & 0xff);
}

static mwb(m, s, off, v)
struct i86 *m;
int s;
i16 off;
int v;
{
	register i16 t;

	s &= 3;
	t = (i16)(m->so[s] + off);
	if (t < m->so[s]) {
		m->fault = 1;
		m->fseg = (i8)s;
		m->foff = off;
		return (0);
	}
	m->sb[s][t] = (char)v;
	return (0);
}

/*
 * i86addr -- the same arithmetic, for the seam rather than for an
 * instruction: the host address of `len' guest bytes, or 0 if they do
 * not all fit in the window.  The BDOS seam hands FCBs and DMA buffers
 * to the native BDOS by address and never copies them, so this is the
 * only thing between a guest DMA offset of 0xFFC0 and our BDOS writing
 * 128 bytes into whatever host segment follows.  len 0 asks only that
 * the offset itself is inside.
 */
char *i86addr(m, s, off, len)
struct i86 *m;
int s;
i16 off;
i32 len;
{
	register i16 t;
	i32 end;

	s &= 3;
	t = (i16)(m->so[s] + off);
	if (t < m->so[s])
		return ((char *)0);
	end = (i32)t + (len ? len - 1 : 0);
	if (end > 0xffffL)
		return ((char *)0);
	return (&m->sb[s][t]);
}

static i16 mrw(m, s, off)
struct i86 *m;
int s;
i16 off;
{
	return ((i16)(mrb(m, s, off) | (mrb(m, s, (i16)(off + 1)) << 8)));
}

static mww(m, s, off, v)
struct i86 *m;
int s;
i16 off, v;
{
	mwb(m, s, off, v & 0xff);
	mwb(m, s, (i16)(off + 1), (v >> 8) & 0xff);
	return (0);
}

/*
 * From here on the four are macros that do an access in the window
 * inline and hand anything else to the function of the same name (a
 * macro does not expand inside itself).  A word fits iff bias + off + 1
 * lands above the bias; a hit on the last byte of a zero-bias segment
 * goes the slow way too, and wraps there.  A cold site calls the
 * function directly as (mrw)(...).
 *
 * `mt' holds the host offset between the check and the access, so no
 * argument may contain another guest memory access.
 */
static i16	mt;

#define MB(m, s, t)	((m)->sb[(s) & 3][(i16)(t)])
#define MO(m, s)	((m)->so[(s) & 3])
#define mrb(m, s, off)	((mt = (i16)(MO(m, s) + (off))) >= MO(m, s) \
			? MB(m, s, mt) & 0xff : mrb(m, s, (i16)(off)))
#define mwb(m, s, off, v) ((mt = (i16)(MO(m, s) + (off))) >= MO(m, s) \
			? (MB(m, s, mt) = (char)(v)) \
			: mwb(m, s, (i16)(off), v))
#define mrw(m, s, off)	((mt = (i16)(MO(m, s) + (off) + 1)) > MO(m, s) \
			? (i16)((MB(m, s, mt - 1) & 0xff) \
				| ((MB(m, s, mt) & 0xff) << 8)) \
			: mrw(m, s, (i16)(off)))
#define mww(m, s, off, v) ((mt = (i16)(MO(m, s) + (off) + 1)) > MO(m, s) \
			? (MB(m, s, mt - 1) = (char)(v), \
			   MB(m, s, mt) = (char)((i16)(v) >> 8)) \
			: mww(m, s, (i16)(off), (i16)(v)))

/* ------------------------------------------------------------------ */
/* flags								*/

/* PF is EVEN parity of the low byte.  A table, because counting the bits
 * costs eight turns of a loop on a path every arithmetic instruction
 * reaches. */
static i8 partab[256] = {
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
 * Materialise the pending lazy record into m->fl and return FLAGS.
 * Idempotent: after it runs, m->lz is LZ_NONE and m->fl is complete.
 *
 * The width mask and sign bit are the only things that differ between a
 * byte and a word operation, and every formula below is written in
 * terms of them.  Casts to i32 make the carry comparisons unsigned in
 * both worlds -- on the target an i16 promotes to unsigned int, on the
 * host to a 32-bit signed int, and the two disagree about `<' unless
 * the comparison is made explicitly wide.
 */
i16 i86flags(m)
struct i86 *m;
{
	register i16 msk, msb;
	i16 f;

	if (m->lz == LZ_NONE)
		return (m->fl);
	i86nflag++;
	msk = (i16)(m->lw ? 0xffff : 0x00ff);
	msb = (i16)(m->lw ? 0x8000 : 0x0080);
	f = 0;
	if ((i16)(m->lr & msk) == 0)
		f |= F_ZF;
	if (m->lr & msb)
		f |= F_SF;
	if (par8(m->lr))
		f |= F_PF;
	switch (m->lz) {
	case LZ_ADD:
		/* r = a + b + c.  The carry out of the top is visible as
		 * the sum having failed to grow; with a carry in, a sum
		 * merely equal to a has already wrapped. */
		if (m->lc ? ((i32)(m->lr & msk) <= (i32)(m->la & msk))
			  : ((i32)(m->lr & msk) <  (i32)(m->la & msk)))
			f |= F_CF;
		if ((m->la ^ m->lb ^ m->lr) & 0x10)
			f |= F_AF;
		if ((i16)((m->la ^ m->lr) & (m->lb ^ m->lr)) & msb)
			f |= F_OF;
		break;
	case LZ_SUB:
		if (m->lc ? ((i32)(m->la & msk) <= (i32)(m->lb & msk))
			  : ((i32)(m->la & msk) <  (i32)(m->lb & msk)))
			f |= F_CF;
		if ((m->la ^ m->lb ^ m->lr) & 0x10)
			f |= F_AF;
		if ((i16)((m->la ^ m->lb) & (m->la ^ m->lr)) & msb)
			f |= F_OF;
		break;
	case LZ_LOG:
		/* CF and OF are cleared; AF is architecturally undefined
		 * and we clear it, which is what the part does. */
		break;
	case LZ_INC:
		/* CF is PRESERVED by INC and DEC -- the one place the
		 * 8086's flag rules are not uniform, and the reason
		 * these are separate classes rather than ADD with b = 1. */
		f |= (i16)(m->fl & F_CF);
		if ((m->la ^ m->lb ^ m->lr) & 0x10)
			f |= F_AF;
		if ((i16)((m->la ^ m->lr) & (m->lb ^ m->lr)) & msb)
			f |= F_OF;
		break;
	case LZ_DEC:
		f |= (i16)(m->fl & F_CF);
		if ((m->la ^ m->lb ^ m->lr) & 0x10)
			f |= F_AF;
		if ((i16)((m->la ^ m->lb) & (m->la ^ m->lr)) & msb)
			f |= F_OF;
		break;
	}
	m->fl = (i16)((m->fl & ~F_LAZY) | f);
	m->lz = LZ_NONE;
	return (m->fl);
}

/* Record a lazy result.  `c' is the carry in for ADC/SBB and 0 else. */
static lazy(m, cls, w, a, b, r, c)
struct i86 *m;
int cls, w, c;
i16 a, b, r;
{
	/* CF must survive: fold first, unless a pending INC or DEC
	 * already left it in m->fl. */
	if ((cls == LZ_INC || cls == LZ_DEC)
	 && m->lz != LZ_INC && m->lz != LZ_DEC)
		i86flags(m);
	m->lz = (i8)cls;
	m->lw = (i8)w;
	m->la = a;
	m->lb = b;
	m->lr = r;
	m->lc = (i8)c;
	return (0);
}

/* ------------------------------------------------------------------ */
/* signed widths						       */

/* Use explicit sign extension for multiply/divide so target 16-bit int
 * and host wider arithmetic produce the same guest values. Mask unsigned
 * intermediates to 32 bits before interpreting their sign. */
static long sx8(v)
int v;
{
	v &= 0xff;
	return ((long)(v & 0x80 ? v - 256 : v));
}

static long sx16(v)
i16 v;
{
	return ((long)((v & 0x8000) ? (long)(v & 0x7fff) - 32768L
				    : (long)v));
}

static long sx32(v)
i32 v;
{
	v &= 0xffffffffL;
	if (!(v & 0x80000000L))
		return ((long)v);
	return ((long)(v & 0x7fffffffL) - 2147483647L - 1L);
}

/* A signed long back into 32 unsigned bits, on either word size. */
static i32 lo32(v)
long v;
{
	return ((i32)v & 0xffffffffL);
}

/* ------------------------------------------------------------------ */
/* operands							       */

/* Read and write the mod r/m operand.  `e' is the effective address,
 * already computed once by the caller so that a read-modify-write does
 * not form it twice (and so that a side effect in the address, were
 * there ever one, could not happen twice). */
static i16 rmrd(m, in, e)
struct i86 *m;
struct i86in *in;
i16 e;
{
	if (in->mod == 3)
		return (in->w ? m->r[in->rm] : (i16)getb(m, in->rm));
	return (in->w ? mrw(m, in->seg, e) : (i16)mrb(m, in->seg, e));
}

static rmwr(m, in, e, v)
struct i86 *m;
struct i86in *in;
i16 e, v;
{
	if (in->mod == 3) {
		if (in->w)
			m->r[in->rm] = v;
		else
			setb(m, in->rm, v & 0xff);
	} else if (in->w)
		mww(m, in->seg, e, v);
	else
		mwb(m, in->seg, e, v & 0xff);
	return (0);
}

/* Inline forms.  One window check serves both widths: a byte whose
 * successor is out of the window goes the slow way, and gets it right. */
#define rmrd(m, in, e)	((in)->mod == 3 \
			? ((in)->w ? (m)->r[(in)->rm] : (i16)getb(m, (in)->rm)) \
			: (mt = (i16)(MO(m, (in)->seg) + (e) + 1)) \
			  > MO(m, (in)->seg) \
			? (i16)((in)->w ? (MB(m, (in)->seg, mt - 1) & 0xff) \
				| ((MB(m, (in)->seg, mt) & 0xff) << 8) \
				: MB(m, (in)->seg, mt - 1) & 0xff) \
			: rmrd(m, in, e))
#define rmwr(m, in, e, v) ((in)->mod == 3 \
			? ((in)->w ? ((m)->r[(in)->rm] = (i16)(v)) \
				   : setb(m, (in)->rm, v)) \
			: (mt = (i16)(MO(m, (in)->seg) + (e) + 1)) \
			  > MO(m, (in)->seg) \
			? (MB(m, (in)->seg, mt - 1) = (char)(v), \
			   (in)->w ? MB(m, (in)->seg, mt) = (char)((i16)(v) >> 8) : 0) \
			: rmwr(m, in, e, v))

/* The reg-field operand, which is always a register. */
#define rgrd(m, in)	((in)->w ? (m)->r[(in)->reg] : (i16)getb(m, (in)->reg))
#define rgwr(m, in, v)	((in)->w ? ((m)->r[(in)->reg] = (i16)(v)) \
				 : setb(m, (in)->reg, v))

/* ------------------------------------------------------------------ */
/* the stack							       */

static push(m, v)
struct i86 *m;
i16 v;
{
	m->r[R_SP] = (i16)(m->r[R_SP] - 2);
	mww(m, S_SS, m->r[R_SP], v);
	return (0);
}

static i16 pop(m)
struct i86 *m;
{
	register i16 v;

	v = mrw(m, S_SS, m->r[R_SP]);
	m->r[R_SP] = (i16)(m->r[R_SP] + 2);
	return (v);
}

/* ------------------------------------------------------------------ */
/* condition codes						       */

/* The sixteen 8086 conditions in encoding order, so the decoder can
 * hand the low four opcode bits straight through.  (f, cc) -> 0 or 1. */
int i86cond(f, cc)
register int f;
int cc;
{
	register int t;

	switch (cc >> 1) {
	case 0: t = (f & F_OF) != 0; break;			/* O	*/
	case 1: t = (f & F_CF) != 0; break;			/* B	*/
	case 2: t = (f & F_ZF) != 0; break;			/* E	*/
	case 3: t = (f & (F_CF | F_ZF)) != 0; break;		/* BE	*/
	case 4: t = (f & F_SF) != 0; break;			/* S	*/
	case 5: t = (f & F_PF) != 0; break;			/* P	*/
	case 6: t = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;   /* L */
	default: t = (((f & F_SF) != 0) != ((f & F_OF) != 0))
			|| (f & F_ZF) != 0; break;		/* LE	*/
	}
	return ((cc & 1) ? !t : t);
}

/*
 * i86cond(i86flags(m), cc) without materialising: each flag the
 * condition needs comes straight from the record by i86flags()' rule
 * for it.  CF of INC/DEC is already in m->fl, as lazy() folded first.
 */
int i86lcond(m, cc)
register struct i86 *m;
int cc;
{
	register i16 msb, a, b, r;
	register int t, c, o;

	if (m->lz == LZ_NONE)
		return (i86cond((int)m->fl, cc));
	if (m->lw) {
		msb = 0x8000;
		a = m->la;
		b = m->lb;
		r = m->lr;
	} else {
		msb = 0x80;
		a = (i16)(m->la & 0xff);
		b = (i16)(m->lb & 0xff);
		r = (i16)(m->lr & 0xff);
	}
	switch (cc >> 1) {
	case 2:						/* E	*/
		t = r == 0;
		break;
	case 4:						/* S	*/
		t = (r & msb) != 0;
		break;
	case 5:						/* P	*/
		t = par8(r);
		break;
	default:
		/* CF, OF and the pairs built from them. */
		switch (m->lz) {
		case LZ_ADD:
			c = m->lc ? r <= a : r < a;
			o = ((a ^ r) & (b ^ r) & msb) != 0;
			break;
		case LZ_SUB:
			c = m->lc ? a <= b : a < b;
			o = ((a ^ b) & (a ^ r) & msb) != 0;
			break;
		case LZ_INC:
			c = (m->fl & F_CF) != 0;
			o = ((a ^ r) & (b ^ r) & msb) != 0;
			break;
		case LZ_DEC:
			c = (m->fl & F_CF) != 0;
			o = ((a ^ b) & (a ^ r) & msb) != 0;
			break;
		default:				/* LZ_LOG */
			c = o = 0;
			break;
		}
		switch (cc >> 1) {
		case 0: t = o; break;				/* O	*/
		case 1: t = c; break;				/* B	*/
		case 3: t = c || r == 0; break;			/* BE	*/
		case 6: t = (r & msb ? 1 : 0) != o; break;	/* L	*/
		default: t = (r & msb ? 1 : 0) != o || r == 0; break; /* LE */
		}
		break;
	}
	return ((cc & 1) ? !t : t);
}

/* ------------------------------------------------------------------ */
/* the ALU eight						       */

/* Returns the result; records the lazy flags; the caller decides
 * whether to store it (CMP does not). */
static i16 alu(m, aop, w, a, b)
struct i86 *m;
int aop, w;
i16 a, b;
{
	register i16 r;
	register int c;

	switch (aop) {
	case 0:					/* ADD			*/
		r = (i16)(a + b);
		lazy(m, LZ_ADD, w, a, b, r, 0);
		break;
	case 1:					/* OR			*/
		r = (i16)(a | b);
		lazy(m, LZ_LOG, w, a, b, r, 0);
		break;
	case 2:					/* ADC			*/
		c = (i86flags(m) & F_CF) != 0;
		r = (i16)(a + b + c);
		lazy(m, LZ_ADD, w, a, b, r, c);
		break;
	case 3:					/* SBB			*/
		c = (i86flags(m) & F_CF) != 0;
		r = (i16)(a - b - c);
		lazy(m, LZ_SUB, w, a, b, r, c);
		break;
	case 4:					/* AND			*/
		r = (i16)(a & b);
		lazy(m, LZ_LOG, w, a, b, r, 0);
		break;
	case 6:					/* XOR			*/
		r = (i16)(a ^ b);
		lazy(m, LZ_LOG, w, a, b, r, 0);
		break;
	default:				/* SUB (5) and CMP (7)	*/
		r = (i16)(a - b);
		lazy(m, LZ_SUB, w, a, b, r, 0);
		break;
	}
	return (r);
}

/*
 * Shifts and rotates, one bit at a time.
 *
 * Iterative on purpose.  This is the slow C path, correctness is the
 * only thing it owes anyone, and the bit-at-a-time form is the one that
 * is obviously right about the carry -- which is the flag every closed
 * form gets wrong at count 0 and at count >= width.  Note also that the
 * 8086 does NOT mask the count to five bits: that arrived with the 186,
 * so a CL of 200 really is two hundred iterations here.
 *
 * A count of zero changes no flags at all, which is why the flag stores
 * are inside the guard.
 */
static i16 shift(m, sop, w, v, n)
struct i86 *m;
int sop, w, n;
i16 v;
{
	register i16 msb, msk;
	register int cf, i, b;
	i16 f, v0;

	msk = (i16)(w ? 0xffff : 0x00ff);
	msb = (i16)(w ? 0x8000 : 0x0080);
	if (n == 0)
		return (v);
	f = i86flags(m);
	cf = (f & F_CF) != 0;
	v0 = (i16)(v & msk);
	v = v0;
	for (i = 0; i < n; i++) {
		switch (sop) {
		case 0:				/* ROL			*/
			b = (v & msb) != 0;
			v = (i16)(((v << 1) | b) & msk);
			cf = b;
			break;
		case 1:				/* ROR			*/
			b = v & 1;
			v = (i16)(((v >> 1) | (b ? msb : 0)) & msk);
			cf = b;
			break;
		case 2:				/* RCL			*/
			b = (v & msb) != 0;
			v = (i16)(((v << 1) | cf) & msk);
			cf = b;
			break;
		case 3:				/* RCR			*/
			b = v & 1;
			v = (i16)(((v >> 1) | (cf ? msb : 0)) & msk);
			cf = b;
			break;
		case 4: case 6:			/* SHL, and SAL = SHL	*/
			cf = (v & msb) != 0;
			v = (i16)((v << 1) & msk);
			break;
		case 5:				/* SHR			*/
			cf = v & 1;
			v = (i16)((v >> 1) & (msk >> 1));
			break;
		default:			/* SAR			*/
			cf = v & 1;
			v = (i16)(((v >> 1) | (v & msb)) & msk);
			break;
		}
	}
	f = (i16)(f & ~F_LAZY);
	if (cf)
		f |= F_CF;
	/* OF is architecturally defined only for a count of one.  The
	 * same formula is applied for larger counts, which is what the
	 * part is observed to do and what any other choice would have to
	 * justify; nothing in the corpus reads OF after a multi-bit
	 * shift. */
	switch (sop) {
	case 0: case 2:				/* ROL, RCL		*/
		if (((v & msb) != 0) != cf)
			f |= F_OF;
		break;
	case 1: case 3:				/* ROR, RCR		*/
		if (((v & msb) != 0) != ((v & (msb >> 1)) != 0))
			f |= F_OF;
		break;
	case 5:					/* SHR: OF from the source */
		if (v0 & msb)
			f |= F_OF;
		break;
	case 7:					/* SAR: OF is always 0	*/
		break;
	default:				/* SHL/SAL		*/
		if (((v & msb) != 0) != cf)
			f |= F_OF;
		break;
	}
	if (sop >= 4) {			/* the shifts, not the rotates,	*/
		if ((i16)(v & msk) == 0)	/* set SF, ZF and PF	*/
			f |= F_ZF;
		if (v & msb)
			f |= F_SF;
		if (par8(v))
			f |= F_PF;
	} else
		f |= (i16)(m->fl & (F_SF | F_ZF | F_PF | F_AF));
	m->fl = f;
	m->lz = LZ_NONE;
	return (v);
}

/* ------------------------------------------------------------------ */
/* string operations						       */

/*
 * MOVS/CMPS/STOS/LODS/SCAS, with the REP prefixes folded in.
 *
 * The source is DS:SI and a segment override moves it; the destination is
 * ES:DI and nothing can move it, so in->seg appears on one side only.  SI
 * and DI step by the operand width, backwards when DF is set, and wrap
 * inside the segment as every other reference here does.  DF is never
 * lazy, so it is read straight out of fl.
 *
 * CMPS and SCAS leave the same lazy record CMP does; the other three
 * touch no flags.  Only the REPE/REPNE early-out needs ZF, so only those
 * two ever materialise it.  A reference that escapes a biased window sets
 * m->fault, which ends the loop and is reported by the caller.
 */
static strop(m, in)
struct i86 *m;
struct i86in *in;
{
	register i16 s, d;
	register int w, rep;
	i16 step, a, b;

	w = in->w;
	step = (i16)(w ? 2 : 1);
	if (m->fl & F_DF)
		step = (i16)(0 - step);
	rep = (in->fl & (IN_REP | IN_REPNE)) != 0;
	for (;;) {
		if (rep && m->r[R_CX] == 0)
			break;
		s = m->r[R_SI];
		d = m->r[R_DI];
		switch (in->x) {
		case 0:					/* MOVS		*/
			if (w) {
				a = mrw(m, in->seg, s);
				mww(m, S_ES, d, a);
			} else {
				a = (i16)mrb(m, in->seg, s);
				mwb(m, S_ES, d, a);
			}
			m->r[R_SI] = (i16)(s + step);
			m->r[R_DI] = (i16)(d + step);
			break;
		case 1:					/* CMPS		*/
			a = (i16)(w ? mrw(m, in->seg, s)
				    : (i16)mrb(m, in->seg, s));
			b = (i16)(w ? mrw(m, S_ES, d) : (i16)mrb(m, S_ES, d));
			alu(m, 7, w, a, b);
			m->r[R_SI] = (i16)(s + step);
			m->r[R_DI] = (i16)(d + step);
			break;
		case 2:					/* STOS		*/
			if (w)
				mww(m, S_ES, d, m->r[R_AX]);
			else
				mwb(m, S_ES, d, getb(m, 0));
			m->r[R_DI] = (i16)(d + step);
			break;
		case 3:					/* LODS		*/
			if (w)
				m->r[R_AX] = mrw(m, in->seg, s);
			else {
				a = (i16)mrb(m, in->seg, s);
				setb(m, 0, a);
			}
			m->r[R_SI] = (i16)(s + step);
			break;
		default:				/* SCAS		*/
			a = (i16)(w ? m->r[R_AX] : (i16)getb(m, 0));
			b = (i16)(w ? mrw(m, S_ES, d) : (i16)mrb(m, S_ES, d));
			alu(m, 7, w, a, b);
			m->r[R_DI] = (i16)(d + step);
			break;
		}
		if (m->fault || !rep)
			break;
		m->r[R_CX] = (i16)(m->r[R_CX] - 1);
		if (in->x == 1 || in->x == 4) {
			a = (i16)((i86flags(m) & F_ZF) != 0);
			if ((in->fl & IN_REPNE) ? a : !a)
				break;
		}
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* segment-register writes					       */

/* Resolve a paragraph inside an assigned segment by storing its byte
 * bias. Otherwise consult i86segnew if installed, then refuse. */
static int setsr(m, s, par)
struct i86 *m;
int s;
i16 par;
{
	register char *b;
	register int i;
	i16 d;

	s &= 3;
	b = i86resolve(par);
	if (b != (char *)0) {			/* the fast path: a base	*/
		m->sr[s] = par;
		m->sb[s] = b;
		m->so[s] = 0;
		return (X_OK);
	}
	for (i = 0; i < i86nseg; i++) {
		if (par < i86spar[i])
			continue;
		d = (i16)(par - i86spar[i]);
		if (d >= CMD_MAXPAR)
			continue;
		i86nsegslow++;
		m->sr[s] = par;
		m->sb[s] = i86sbase[i];
		m->so[s] = (i16)(d * CMD_PARA);
		return (X_OK);
	}
	if (i86segnew) {
		b = (*i86segnew)(par);
		if (b != (char *)0) {
			i86nsegslow++;
			m->sr[s] = par;
			m->sb[s] = b;
			m->so[s] = 0;
			return (X_OK);
		}
	}
	i86nsegbad++;
	i86segbad = par;
	return (X_SEGESC);
}

/* i86addr() for a far pointer, resolved as a segment load would be. */
char *i86far(m, par, off, len)
struct i86 *m;
i16 par, off;
i32 len;
{
	char *b, *p;
	i16 r, o;

	r = m->sr[S_ES];
	b = m->sb[S_ES];
	o = m->so[S_ES];
	p = setsr(m, S_ES, par) == X_OK ? i86addr(m, S_ES, off, len)
	    : (char *)0;
	m->sr[S_ES] = r;
	m->sb[S_ES] = b;
	m->so[S_ES] = o;
	return (p);
}

/* ------------------------------------------------------------------ */
/* interrupts the guest itself handles				       */

/*
 * takeint -- enter the guest's own handler for vector `n', the way the
 * hardware does: FLAGS, CS and IP pushed, IF and TF cleared, control
 * through the four bytes at 0000:4n.
 *
 * Two things make it a no-op instead, and both return X_INT so that the
 * caller hands the vector to the seam exactly as it always has.  The
 * first is no paragraph 0 at all -- a caller that placed no segment
 * there has no vector table and no guest handlers.  The second is a
 * zero vector, which is this shim's spelling of "nobody installed one":
 * 0000:0000 is the table's own first word, so no handler is ever really
 * there, and leaving the vector zero is what lets INT 0E0h keep going
 * to the seam while a vector the guest DID write is honoured.
 */
static int takeint(m, n)
struct i86 *m;
int n;
{
	register char *v;
	i16 off, seg, sp0;
	int rc;

	v = i86resolve((i16)0);
	if (v == (char *)0)
		return (X_INT);
	v += (n & 0xff) * 4;
	off = (i16)((v[0] & 0xff) | ((i16)(v[1] & 0xff) << 8));
	seg = (i16)((v[2] & 0xff) | ((i16)(v[3] & 0xff) << 8));
	if (off == 0 && seg == 0)
		return (X_INT);
	sp0 = m->r[R_SP];
	push(m, (i16)(i86flags(m) | F_ONES));
	push(m, m->sr[S_CS]);
	push(m, m->ip);
	rc = setsr(m, S_CS, seg);
	if (rc != X_OK) {
		m->r[R_SP] = sp0;
		return (rc);
	}
	m->fl = (i16)(m->fl & ~(F_IF | F_TF));
	m->ip = off;
	return (X_OK);
}

/* A far transfer to entry SS:0000 is the guest warm-boot convention.
 * wset enables this environment rule; CPU-only callers can leave it off. */
static int wboot(m, seg, off)
struct i86 *m;
i16 seg, off;
{
	return (m->wset && off == 0 && seg == m->wseg);
}

/*
 * A register DEC at ip0 that JNZ ip0 follows is a delay loop.  With r the
 * count left after the DEC just run, finish it in one step, leaving the
 * state and counts its 2r+1 remaining instructions would, then sleep the
 * time the whole loop takes on a real 8086.
 */
static delay(m, in, ip0, r)
struct i86 *m;
struct i86in *in;
i16 ip0, r;
{
	register char *cs;
	register i32 n, t;
	int op, len;

	cs = m->sb[S_CS];
	op = cs[ip0] & 0xff;
	if (op >= 0x48 && op <= 0x4f) {
		len = 1;
		t = C_DECR;
	} else if ((op & 0xfe) == 0xfe
		&& (cs[(i16)(ip0 + 1)] & 0xf8) == 0xc8) {
		len = 2;
		t = C_DECRM;
	} else
		return (0);
	if ((cs[(i16)(ip0 + len)] & 0xff) != 0x75
	 || (cs[(i16)(ip0 + len + 1)] & 0xff) != ((-(len + 2)) & 0xff))
		return (0);
	n = (i32)r;
	t = (n + 1) * t + n * C_JCCY + C_JCCN;
	/* Neither the jumps nor a DEC after a DEC materialise. */
	if (in->w)
		m->r[in->rm] = 0;
	else
		setb(m, in->rm, 0);
	lazy(m, LZ_DEC, in->w, (i16)1, (i16)1, (i16)0, 0);
	i86ninsn += 2 * n + 1;
	i86nskip += 2 * n + 1;
	m->ip = (i16)(ip0 + len + 2);
	if (!i86wait)
		return (0);
	owed += t;
	if (owed >= I86TICK) {
		n = owed / I86TICK;
		owed -= n * I86TICK;
		/* A wait of k ticks can end just after k - 1. */
		(*i86wait)((int)n + 1);
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* one instruction						       */

/*
 * i86step -- decode and execute the instruction at CS:IP.
 *
 * IP is advanced past the instruction BEFORE it executes, so that a
 * relative branch target (computed at decode time from the same base)
 * and a pushed return address agree with the hardware without either
 * having to know the instruction's length.  A refusal leaves IP where
 * it was, so the caller can report the address that could not run.
 */
int i86step(m, in)
struct i86 *m;
struct i86in *in;
{
	i86nrun = 1;
	return (i86run(m, in));
}

/*
 * i86run -- step until i86nrun reaches 0 or an instruction returns
 * other than X_OK.  i86nrun counts the X_OK steps down.
 */
int i86run(m, in)
register struct i86 *m;
register struct i86in *in;
{
	register i16 a, b, r;
	i16 e, ip0, tf0;
	i32 la, lb;
	long sa, sb;
	int rc, n, ient;

next:
	ip0 = m->ip;
	ient = 0;
	m->fault = 0;
	/* A code segment the slow path had to bias is refused before a
	 * byte of it is decoded, and this is not laziness.  i86dec()
	 * addresses its bytes as cs[(i16)(ip + k)] -- it wraps at the
	 * guest's 64 KB, which is right -- so handing it a base partway
	 * into a host segment lets a decode at the top of the guest's
	 * segment read past the end of the host's.  There is no correct
	 * answer available: the bytes the guest means are in another
	 * segment.  Stage one refuses; the alternative is reading memory
	 * that belongs to something else.  No DRI file reaches here. */
	if (m->so[S_CS]) {
		m->fault = 1;
		m->fseg = S_CS;
		m->foff = m->ip;
		return (X_WINDOW);
	}
	i86dec(m->sb[S_CS], m->ip, in);
	if (in->op == I_BAD)
		return (X_BAD);
	i86ninsn++;
	/* The trap flag is read HERE and acted on at the end, which is
	 * what makes an IRET or POPF that sets it step the instruction
	 * AFTER itself rather than trapping on the spot. */
	tf0 = (i16)(m->fl & F_TF);
	m->ip = (i16)(m->ip + in->len);
	/* The effective address of a mod r/m memory operand.  Computed here
	 * because it depends on live registers; the decode does not. */
	e = 0;
	if (!(in->fl & IN_MEM))
		goto exec;
	if (in->mod == 0 && in->rm == 6) {
		e = in->disp;
		goto exec;
	}
	switch (in->rm) {
	case 0:  a = (i16)(m->r[R_BX] + m->r[R_SI]); break;
	case 1:  a = (i16)(m->r[R_BX] + m->r[R_DI]); break;
	case 2:  a = (i16)(m->r[R_BP] + m->r[R_SI]); break;
	case 3:  a = (i16)(m->r[R_BP] + m->r[R_DI]); break;
	case 4:  a = m->r[R_SI]; break;
	case 5:  a = m->r[R_DI]; break;
	case 6:  a = m->r[R_BP]; break;
	default: a = m->r[R_BX]; break;
	}
	e = (i16)(a + in->disp);
exec:

	switch (in->op) {

	case I_ALU:
		if (in->fl & IN_IMM) {
			a = rmrd(m, in, e);
			b = in->imm;
		} else if (in->fl & IN_DIR) {
			a = rgrd(m, in);
			b = rmrd(m, in, e);
		} else {
			a = rmrd(m, in, e);
			b = rgrd(m, in);
		}
		r = alu(m, in->x, in->w, a, b);
		if (in->x == 7)			/* CMP stores nothing	*/
			break;
		if (in->fl & IN_DIR)
			rgwr(m, in, r);
		else
			rmwr(m, in, e, r);
		break;

	case I_TEST:
		a = rmrd(m, in, e);
		b = (i16)((in->fl & IN_IMM) ? in->imm : rgrd(m, in));
		alu(m, 4, in->w, a, b);		/* AND, result discarded */
		break;

	case I_MOV:
		if (in->fl & IN_IMM)
			rmwr(m, in, e, in->imm);
		else if (in->fl & IN_DIR) {
			a = rmrd(m, in, e);
			rgwr(m, in, a);
		} else {
			a = rgrd(m, in);
			rmwr(m, in, e, a);
		}
		break;

	case I_MOVSR:
		if (in->fl & IN_DIR) {		/* 8E: Sreg <- r/m	*/
			if (in->x == S_CS) {
				m->ip = ip0;		/* MOV CS,x is not
							 * an instruction */
				return (X_UNIMP);
			}
			rc = setsr(m, in->x, (rmrd)(m, in, e));
			if (rc != X_OK) {
				m->ip = ip0;
				return (rc);
			}
		} else				/* 8C: r/m <- Sreg	*/
			(rmwr)(m, in, e, m->sr[in->x]);
		break;

	case I_LEA:
		if (!(in->fl & IN_MEM))
			return (X_BAD);		/* LEA of a register	*/
		m->r[in->reg] = e;
		break;

	case I_LXS:
		if (!(in->fl & IN_MEM))
			return (X_BAD);
		a = (mrw)(m, in->seg, e);
		b = (mrw)(m, in->seg, (i16)(e + 2));
		rc = setsr(m, in->x, b);
		if (rc != X_OK) {
			m->ip = ip0;
			return (rc);
		}
		m->r[in->reg] = a;
		break;

	case I_XCHG:
		a = rmrd(m, in, e);
		b = rgrd(m, in);
		rmwr(m, in, e, b);
		rgwr(m, in, a);
		break;

	case I_INC:
		a = rmrd(m, in, e);
		r = (i16)(a + 1);
		lazy(m, LZ_INC, in->w, a, 1, r, 0);
		rmwr(m, in, e, r);
		break;
	case I_DEC:
		a = rmrd(m, in, e);
		r = (i16)(a - 1);
		lazy(m, LZ_DEC, in->w, a, 1, r, 0);
		rmwr(m, in, e, r);
		if (in->mod == 3 && !tf0 && i86fast
		 && (in->w ? r : r & 0xff) != 0)
			delay(m, in, ip0, in->w ? r : (i16)(r & 0xff));
		break;
	case I_NOT:				/* NOT affects no flags	*/
		a = rmrd(m, in, e);
		rmwr(m, in, e, (i16)~a);
		break;
	case I_NEG:
		b = rmrd(m, in, e);
		r = (i16)(0 - b);
		lazy(m, LZ_SUB, in->w, 0, b, r, 0);
		rmwr(m, in, e, r);
		break;

	case I_SHIFT:
		n = (int)(in->imm2 ? (m->r[R_CX] & 0xff) : 1);
		a = rmrd(m, in, e);
		r = shift(m, in->x, in->w, a, n);
		rmwr(m, in, e, r);
		break;

	case I_PUSH:
		/* PUSH SP on an 8086 pushes the value AFTER the
		 * decrement -- the 286 changed that, and period code
		 * that cares is code that would notice. */
		if (in->mod == 3 && in->rm == R_SP && !(in->fl & IN_MEM)) {
			m->r[R_SP] = (i16)(m->r[R_SP] - 2);
			mww(m, S_SS, m->r[R_SP], m->r[R_SP]);
		} else
			push(m, rmrd(m, in, e));
		break;
	case I_POP:
		r = pop(m);
		rmwr(m, in, e, r);
		break;
	case I_PUSHSR:
		push(m, m->sr[in->x]);
		break;
	case I_POPSR:
		if (in->x == S_CS) {
			m->ip = ip0;
			return (X_UNIMP);
		}
		a = (mrw)(m, S_SS, m->r[R_SP]);
		rc = setsr(m, in->x, a);
		if (rc != X_OK) {
			m->ip = ip0;
			return (rc);
		}
		m->r[R_SP] = (i16)(m->r[R_SP] + 2);
		break;
	case I_PUSHF:
		push(m, (i16)(i86flags(m) | F_ONES));
		break;
	case I_POPF:
		m->fl = (i16)(pop(m) | F_ONES);
		m->lz = LZ_NONE;
		break;

	case I_JMP:
		m->ip = in->disp;
		break;
	case I_JCC:
		if (i86lcond(m, in->x))
			m->ip = in->disp;
		break;
	case I_LOOP:
		if (in->x == 3) {		/* JCXZ, which does not	*/
			if (m->r[R_CX] == 0)	/* decrement CX		*/
				m->ip = in->disp;
			break;
		}
		m->r[R_CX] = (i16)(m->r[R_CX] - 1);
		if (m->r[R_CX] == 0)
			break;
		if (in->x == 2				/* LOOP		*/
		 || (in->x == 1 && i86lcond(m, 4))		/* LOOPE	*/
		 || (in->x == 0 && i86lcond(m, 5)))		/* LOOPNE */
			m->ip = in->disp;
		break;
	case I_CALL:
		push(m, m->ip);
		m->ip = in->disp;
		break;
	case I_RET:
		m->ip = pop(m);
		if (in->fl & IN_IMM)
			m->r[R_SP] = (i16)(m->r[R_SP] + in->imm);
		break;
	/* Resolve far destinations before changing stack state. Recognize
	 * entry SS:0000 as warm boot; otherwise validate the new CS paragraph. */
	case I_JMPI:
		if (in->x) {			/* far, through memory	*/
			if (!(in->fl & IN_MEM))
				return (X_BAD);	/* no far JMP of a reg	*/
			a = (mrw)(m, in->seg, e);		/* offset */
			b = (mrw)(m, in->seg, (i16)(e + 2));	/* segment */
			if (wboot(m, b, a)) {
				m->ip = ip0;
				return (X_WBOOT);
			}
			rc = setsr(m, S_CS, b);
			if (rc != X_OK) {
				m->ip = ip0;
				return (rc);
			}
			m->ip = a;
			break;
		}
		m->ip = rmrd(m, in, e);
		break;
	case I_CALLI:
		if (in->x) {			/* far, through memory	*/
			if (!(in->fl & IN_MEM))
				return (X_BAD);
			a = (mrw)(m, in->seg, e);
			b = (mrw)(m, in->seg, (i16)(e + 2));
			if (wboot(m, b, a)) {
				m->ip = ip0;
				return (X_WBOOT);
			}
			r = m->sr[S_CS];	/* the return segment	*/
			rc = setsr(m, S_CS, b);
			if (rc != X_OK) {
				m->ip = ip0;
				return (rc);
			}
			push(m, r);
			push(m, m->ip);
			m->ip = a;
			break;
		}
		a = rmrd(m, in, e);
		push(m, m->ip);
		m->ip = a;
		break;
	case I_JMPF:				/* EA: far direct	*/
		if (wboot(m, in->imm2, in->imm)) {
			m->ip = ip0;
			return (X_WBOOT);
		}
		rc = setsr(m, S_CS, in->imm2);
		if (rc != X_OK) {
			m->ip = ip0;
			return (rc);
		}
		m->ip = in->imm;
		break;
	case I_CALLF:				/* 9A: far direct	*/
		if (wboot(m, in->imm2, in->imm)) {
			m->ip = ip0;
			return (X_WBOOT);
		}
		r = m->sr[S_CS];
		rc = setsr(m, S_CS, in->imm2);
		if (rc != X_OK) {
			m->ip = ip0;
			return (rc);
		}
		push(m, r);
		push(m, m->ip);
		m->ip = in->imm;
		break;
	case I_RETF:
		/* The frame is READ before it is popped, so that a warm
		 * boot or an unresolvable segment leaves SP where the
		 * instruction found it. */
		a = (mrw)(m, S_SS, m->r[R_SP]);			/* offset */
		b = (mrw)(m, S_SS, (i16)(m->r[R_SP] + 2));	/* segment */
		if (wboot(m, b, a)) {
			m->ip = ip0;
			return (X_WBOOT);
		}
		rc = setsr(m, S_CS, b);
		if (rc != X_OK) {
			m->ip = ip0;
			return (rc);
		}
		m->r[R_SP] = (i16)(m->r[R_SP] + 4);
		if (in->fl & IN_IMM)
			m->r[R_SP] = (i16)(m->r[R_SP] + in->imm);
		m->ip = a;
		break;

	case I_INT:
		i86intno = (int)(in->imm & 0xff);
		rc = takeint(m, i86intno);
		if (rc == X_INT)
			return (X_INT);	/* IP past the INT: the seam's rule */
		if (rc != X_OK) {
			m->ip = ip0;
			return (rc);
		}
		ient = 1;
		break;
	case I_IRET:
		/* The frame is READ before SP moves, on I_RETF's pattern,
		 * so an unresolvable CS leaves the stack as it was. */
		a = (mrw)(m, S_SS, m->r[R_SP]);			/* ip	*/
		b = (mrw)(m, S_SS, (i16)(m->r[R_SP] + 2));	/* cs	*/
		r = (mrw)(m, S_SS, (i16)(m->r[R_SP] + 4));	/* flags */
		if (m->fault)
			break;
		rc = setsr(m, S_CS, b);
		if (rc != X_OK) {
			m->ip = ip0;
			return (rc);
		}
		m->r[R_SP] = (i16)(m->r[R_SP] + 6);
		m->ip = a;
		m->fl = (i16)(r | F_ONES);
		m->lz = LZ_NONE;
		break;

	case I_CBW:
		m->r[R_AX] = (i16)((m->r[R_AX] & 0x80) ? (m->r[R_AX] | 0xff00)
						       : (m->r[R_AX] & 0x00ff));
		break;
	case I_CWD:
		m->r[R_DX] = (i16)((m->r[R_AX] & 0x8000) ? 0xffff : 0);
		break;

	case I_LAHF:
		a = i86flags(m);
		setb(m, 4, a & 0xff);			/* AH		*/
		break;
	case I_SAHF:
		i86flags(m);
		m->fl = (i16)((m->fl & 0xff00) | (getb(m, 4) & 0x00d5) | 0x02);
		break;

	case I_FLAG:
		i86flags(m);
		if (in->x == 0)
			m->fl = (i16)(m->fl & ~in->imm);
		else if (in->x == 1)
			m->fl = (i16)(m->fl | in->imm);
		else
			m->fl = (i16)(m->fl ^ in->imm);
		break;

	case I_XLAT:
		a = (i16)(m->r[R_BX] + (getb(m, 0) & 0xff));
		a = (i16)mrb(m, in->seg, a);
		setb(m, 0, a);
		break;

	case I_MULDIV:
		/*
		 * The four widening forms.  Every intermediate is held
		 * as a `long' with no more than 32 bits of meaning, and
		 * every store back into a guest register goes through
		 * lo32() and an explicit mask -- see the sx8/sx16/sx32
		 * comment above for why a cast would not survive the
		 * move between a 16-bit-int target and a 64-bit host.
		 *
		 * Division truncates toward zero on the 8086.  C89
		 * leaves the sign of a negative quotient to the
		 * implementation; every compiler this has to run under
		 * truncates toward zero, and the tests assert it with
		 * -100/7 rather than assuming it.
		 */
		a = rmrd(m, in, e);
		i86flags(m);
		switch (in->x) {
		case 4:				/* MUL			*/
			if (in->w) {
				la = (i32)m->r[R_AX] * (i32)a;
				m->r[R_AX] = (i16)(la & 0xffffL);
				m->r[R_DX] = (i16)((la >> 16) & 0xffffL);
				b = m->r[R_DX];
			} else {
				la = (i32)(getb(m, 0) & 0xff)
				   * (i32)(a & 0xff);
				m->r[R_AX] = (i16)(la & 0xffffL);
				b = (i16)((la >> 8) & 0xff);
			}
			m->fl &= ~(F_CF | F_OF);
			if (b)			/* the high half is not	*/
				m->fl |= (F_CF | F_OF);	/* all zero	*/
			break;
		case 5:				/* IMUL			*/
			if (in->w) {
				sa = sx16(m->r[R_AX]) * sx16(a);
				la = lo32(sa);
				m->r[R_AX] = (i16)(la & 0xffffL);
				m->r[R_DX] = (i16)((la >> 16) & 0xffffL);
				sb = sx16(m->r[R_AX]);
			} else {
				sa = sx8(getb(m, 0)) * sx8((int)a);
				la = lo32(sa);
				m->r[R_AX] = (i16)(la & 0xffffL);
				sb = sx8(getb(m, 0));
			}
			m->fl &= ~(F_CF | F_OF);
			if (sa != sb)		/* the high half is not	*/
				m->fl |= (F_CF | F_OF);	/* a sign extension */
			break;
		case 6:				/* DIV			*/
			if (in->w) {
				if (a == 0)
					goto divzero;
				la = ((((i32)m->r[R_DX]) << 16)
				    | (i32)m->r[R_AX]) & 0xffffffffL;
				lb = la / (i32)a;
				if (lb > 0xffffL)
					goto divzero;
				m->r[R_DX] = (i16)(la % (i32)a);
				m->r[R_AX] = (i16)lb;
			} else {
				if ((a & 0xff) == 0)
					goto divzero;
				la = (i32)m->r[R_AX] & 0xffffL;
				lb = la / (i32)(a & 0xff);
				if (lb > 0xffL)
					goto divzero;
				setb(m, 4, (int)(la % (i32)(a & 0xff)));
				setb(m, 0, (int)lb);
			}
			break;
		default:			/* IDIV			*/
			if (in->w) {
				sb = sx16(a);
				if (sb == 0)
					goto divzero;
				sa = sx32(((((i32)m->r[R_DX]) << 16)
					 | (i32)m->r[R_AX]) & 0xffffffffL);
				if (sa / sb > 32767L || sa / sb < -32768L)
					goto divzero;
				m->r[R_DX] = (i16)(lo32(sa % sb) & 0xffffL);
				m->r[R_AX] = (i16)(lo32(sa / sb) & 0xffffL);
			} else {
				sb = sx8((int)a);
				if (sb == 0)
					goto divzero;
				sa = sx16(m->r[R_AX]);
				if (sa / sb > 127L || sa / sb < -128L)
					goto divzero;
				setb(m, 4, (int)(lo32(sa % sb) & 0xffL));
				setb(m, 0, (int)(lo32(sa / sb) & 0xffL));
			}
			break;
		}
		m->lz = LZ_NONE;
		break;

	divzero:
		/* A divide error is INT 0.  An 8086 pushes the address
		 * AFTER the divide; with no interrupt machinery here the
		 * caller gets the vector with IP back at the instruction,
		 * the address a refusal has to name. */
		m->ip = ip0;
		m->lz = LZ_NONE;
		i86intno = 0;
		return (X_INT);

	case I_DAA:
	case I_DAS:
		/* The two decimal adjusts, written out rather than
		 * mapped onto the Z8000's DAB, because DAB reads the
		 * host's D and H flags and this path has neither -- it
		 * has an 8086 AF it has just materialised. */
		i86flags(m);
		a = (i16)(getb(m, 0) & 0xff);
		r = a;
		b = (i16)(m->fl & F_CF);
		if ((a & 0x0f) > 9 || (m->fl & F_AF)) {
			r = (i16)(in->op == I_DAA ? (r + 6) : (r - 6));
			m->fl |= F_AF;
		} else
			m->fl &= ~F_AF;
		if (a > 0x99 || b) {
			r = (i16)(in->op == I_DAA ? (r + 0x60) : (r - 0x60));
			m->fl |= F_CF;
		} else
			m->fl &= ~F_CF;
		setb(m, 0, r & 0xff);
		m->fl &= ~(F_ZF | F_SF | F_PF);
		if ((r & 0xff) == 0)
			m->fl |= F_ZF;
		if (r & 0x80)
			m->fl |= F_SF;
		if (par8(r))
			m->fl |= F_PF;
		m->lz = LZ_NONE;
		break;

	case I_AAA:
	case I_AAS:
		i86flags(m);
		if ((getb(m, 0) & 0x0f) > 9 || (m->fl & F_AF)) {
			setb(m, 0, in->op == I_AAA ? (getb(m, 0) + 6)
						   : (getb(m, 0) - 6));
			setb(m, 4, in->op == I_AAA ? (getb(m, 4) + 1)
						   : (getb(m, 4) - 1));
			m->fl |= (F_AF | F_CF);
		} else
			m->fl &= ~(F_AF | F_CF);
		setb(m, 0, getb(m, 0) & 0x0f);
		m->lz = LZ_NONE;
		break;

	case I_AAM:
		if ((in->imm & 0xff) == 0) {
			m->ip = ip0;
			i86intno = 0;
			return (X_INT);
		}
		a = (i16)(getb(m, 0) & 0xff);
		setb(m, 4, (int)(a / (in->imm & 0xff)));
		setb(m, 0, (int)(a % (in->imm & 0xff)));
		lazy(m, LZ_LOG, 0, 0, 0, (i16)(getb(m, 0) & 0xff), 0);
		break;
	case I_AAD:
		a = (i16)((getb(m, 4) & 0xff) * (in->imm & 0xff)
			+ (getb(m, 0) & 0xff));
		setb(m, 0, a & 0xff);
		setb(m, 4, 0);
		lazy(m, LZ_LOG, 0, 0, 0, (i16)(a & 0xff), 0);
		break;

	case I_NOP:
		break;
	case I_HLT:
		m->halt = 1;
		m->ip = ip0;
		return (X_HALT);

	case I_STRING:
		strop(m, in);
		break;

	/* ---- decoded but not executed; decoding them buys a loud
	 * refusal that names the instruction. */
	case I_INTO:				/* no overflow trap	*/
	case I_ESC:				/* no 8087		*/
	case I_IO:				/* no PC hardware	*/
	case I_WAIT:
	default:
		m->ip = ip0;
		return (X_UNIMP);
	}
	/* A window fault is caught at the reference and reported here, so
	 * the instruction has already had its effect up to the escaping
	 * byte.  The guest is not resumed, and the fault carries the slot
	 * and offset so the refusal can name them. */
	if (m->fault) {
		m->ip = ip0;
		return (X_WINDOW);
	}
	/* Single step.  The instruction that entered a handler cleared TF
	 * on the way in and must not also be stepped, and a vector 1 the
	 * guest never wrote leaves TF meaning nothing, as it always did. */
	if (tf0 && !ient) {
		rc = takeint(m, 1);
		if (rc != X_OK && rc != X_INT) {
			m->ip = ip0;
			return (rc);
		}
		if (m->fault) {
			m->ip = ip0;
			return (X_WINDOW);
		}
	}
	if (--i86nrun)
		goto next;
	return (X_OK);
}
