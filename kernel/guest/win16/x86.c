/*
 * x86.c -- the 80386 integer core of the Win16 environment.
 *
 * One instruction at a time through a switch on the opcode.  Guest
 * memory is read and written a byte at a time in guest order, so the
 * host's byte order does not matter.  Arithmetic flags are made only
 * when something reads them: an ALU instruction keeps its operands
 * and result (lf_*), and x86_flags() or a condition derives the flags.
 *
 * A fault inside an instruction longjmps back to the run loop, which
 * puts IP back at the instruction and delivers the exception: through
 * the interrupt vector table in real mode (the instruction tests), to
 * the host's fault handler in protected mode.  Instructions check
 * before they write, so a restarted one sees what it saw the first
 * time (string instructions keep their progress, as on the 386).
 */

#include <setjmp.h>
#include <string.h>
#include "x86.h"

#define	THUNK_OP	0xff	/* 0F FF lo hi */

int x86_retthunk = 0xffff;

static struct x86 *cpu;
static u8 *mem;
static jmp_buf *trapjb;
static int xnum, xerr;

/* decoding */
static int m_mod, m_reg, m_rm, m_seg;
static u32 m_off;

static u8 parity[256];
static const u32 szmask[3] = { 0xff, 0xffff, 0xffffffff };
static const u32 sztop[3] = { 0x80, 0x8000, 0x80000000 };

/* raise exception n now; IP goes back to the instruction */
static void
fault(n, err)
	int n, err;
{
	xnum = n;
	xerr = err;
	longjmp(*trapjb, 1);
}

void
x86_exception(c, n, err)
	struct x86 *c;
	int n, err;
{
	fault(n, err);
}

/* ---- flags ---- */

static u32
getcf()
{
	u32 d = cpu->lf_dst, s = cpu->lf_src, r = cpu->lf_res, t = sztop[cpu->lf_sz];

	switch (cpu->lf_op) {
	case LF_ADD:
		return ((d & s) | ((d | s) & ~r)) & t ? 1 : 0;
	case LF_SUB:
		return ((~d & s) | ((~d | s) & r)) & t ? 1 : 0;
	case LF_LOGIC:
		return 0;
	}
	return cpu->fl & F_CF;
}

static u32
getzf()
{
	if (cpu->lf_op == LF_NONE)
		return cpu->fl & F_ZF;
	return (cpu->lf_res & szmask[cpu->lf_sz]) ? 0 : F_ZF;
}

static u32
getsf()
{
	if (cpu->lf_op == LF_NONE)
		return cpu->fl & F_SF;
	return (cpu->lf_res & sztop[cpu->lf_sz]) ? F_SF : 0;
}

static u32
getof()
{
	u32 d = cpu->lf_dst, s = cpu->lf_src, r = cpu->lf_res, t = sztop[cpu->lf_sz];

	switch (cpu->lf_op) {
	case LF_ADD:
	case LF_INC:
		return ((d ^ r) & (s ^ r)) & t ? F_OF : 0;
	case LF_SUB:
	case LF_DEC:
		return ((d ^ s) & (d ^ r)) & t ? F_OF : 0;
	case LF_LOGIC:
		return 0;
	}
	return cpu->fl & F_OF;
}

static u32
getpf()
{
	if (cpu->lf_op == LF_NONE)
		return cpu->fl & F_PF;
	return parity[cpu->lf_res & 0xff];
}

static u32
getaf()
{
	switch (cpu->lf_op) {
	case LF_NONE:
		return cpu->fl & F_AF;
	case LF_LOGIC:
		return 0;
	}
	return (cpu->lf_dst ^ cpu->lf_src ^ cpu->lf_res) & F_AF;
}

u32
x86_flags(c)
	struct x86 *c;
{
	struct x86 *o = cpu;
	u32 f;

	cpu = c;
	if (c->lf_op != LF_NONE) {
		f = c->fl & ~F_ARITH;
		f |= getcf() | getpf() | getaf() | getzf() | getsf() | getof();
		c->fl = f;
		c->lf_op = LF_NONE;
	}
	cpu = o;
	return c->fl;
}

void
x86_setflags(c, v)
	struct x86 *c;
	u32 v;
{
	c->fl = v | 2;
	c->lf_op = LF_NONE;
}

#define	FLAGS()		x86_flags(cpu)
#define	SETF(v)		(cpu->fl = (v) | 2, cpu->lf_op = LF_NONE)

/* condition n of Jcc/SETcc */
static int
cond(n)
	int n;
{
	int t;

	switch (n >> 1) {
	case 0: t = getof() != 0; break;
	case 1: t = getcf() != 0; break;
	case 2: t = getzf() != 0; break;
	case 3: t = getcf() || getzf(); break;
	case 4: t = getsf() != 0; break;
	case 5: t = getpf() != 0; break;
	case 6: t = !getsf() != !getof(); break;
	default: t = getzf() || !getsf() != !getof(); break;
	}
	return t ^ (n & 1);
}

/* set lazy flags */
#define	LAZY(op, sz, d, s, r) \
	(cpu->lf_op = (op), cpu->lf_sz = (sz), cpu->lf_dst = (d), cpu->lf_src = (s), cpu->lf_res = (r))

/* make the arithmetic flags eager, so some can be set by hand */
static void
eager()
{
	if (cpu->lf_op != LF_NONE)
		FLAGS();
}

static void
setfl(mask, on)
	u32 mask;
	int on;
{
	eager();
	if (on)
		cpu->fl |= mask;
	else
		cpu->fl &= ~mask;
}

/* SF, ZF, PF from a result of size sz; the other arithmetic flags kept */
static void
szp(sz, r)
	int sz;
	u32 r;
{
	eager();
	r &= szmask[sz];
	cpu->fl &= ~(F_SF | F_ZF | F_PF);
	cpu->fl |= parity[r & 0xff] | (r ? 0 : F_ZF) | (r & sztop[sz] ? F_SF : 0);
}

/* ---- registers ---- */

static u32
getreg(sz, i)
	int sz, i;
{
	switch (sz) {
	case 0:
		return i < 4 ? cpu->r[i] & 0xff : cpu->r[i - 4] >> 8 & 0xff;
	case 1:
		return cpu->r[i] & 0xffff;
	}
	return cpu->r[i];
}

static void
setreg(sz, i, v)
	int sz, i;
	u32 v;
{
	switch (sz) {
	case 0:
		if (i < 4)
			cpu->r[i] = (cpu->r[i] & ~0xff) | (v & 0xff);
		else
			cpu->r[i - 4] = (cpu->r[i - 4] & ~0xff00) | (v & 0xff) << 8;
		break;
	case 1:
		cpu->r[i] = (cpu->r[i] & ~0xffff) | (v & 0xffff);
		break;
	default:
		cpu->r[i] = v;
	}
}

/* ---- memory ---- */

/* linear address of n bytes at sr:off, or a fault */
static u32
lin(sr, off, n, wr)
	int sr;
	u32 off;
	int n, wr;
{
	struct seg *s = &cpu->s[sr];
	u32 a, end = off + n - 1;
	int x = sr == S_SS ? X_SS : X_GP;

	if (cpu->prot) {
		if (s->acc == 0)
			fault(X_GP, 0);
		if (wr) {
			if ((s->acc & (D_CODE | D_W)) != D_W)
				fault(x, 0);
		} else if ((s->acc & (D_CODE | D_R)) == D_CODE)
			fault(X_GP, 0);
	}
	if (!(s->acc & D_CODE) && (s->acc & D_EXPD)) {
		if (off <= s->limit || end > (s->big ? 0xffffffff : 0xffff) || end < off)
			fault(x, 0);
	} else if (end > s->limit || end < off)
		fault(x, 0);
	a = s->base + off;
	if (a + n > cpu->memsize || a + n < a)
		fault(x, 0);
	return a;
}

static u32
rd(sr, off, sz)
	int sr, sz;
	u32 off;
{
	u32 a;

	switch (sz) {
	case 0:
		a = lin(sr, off, 1, 0);
		return mem[a];
	case 1:
		a = lin(sr, off, 2, 0);
		return RD16(mem, a);
	}
	a = lin(sr, off, 4, 0);
	return RD32(mem, a);
}

static void
wr(sr, off, sz, v)
	int sr, sz;
	u32 off, v;
{
	u32 a;

	switch (sz) {
	case 0:
		a = lin(sr, off, 1, 1);
		mem[a] = v;
		break;
	case 1:
		a = lin(sr, off, 2, 1);
		WR16(mem, a, v);
		break;
	default:
		a = lin(sr, off, 4, 1);
		WR32(mem, a, v);
	}
}

/* ---- code fetch ---- */

static u32
fetch8()
{
	struct seg *s = &cpu->s[S_CS];
	u32 a;

	if (cpu->eip > s->limit || (a = s->base + cpu->eip) >= cpu->memsize)
		fault(X_GP, 0);
	cpu->eip++;
	if (cpu->eip - cpu->ip0 > 15)
		fault(X_GP, 0);
	return mem[a];
}

static u32
fetch16()
{
	u32 v = fetch8();

	return v | fetch8() << 8;
}

static u32
fetch32()
{
	u32 v = fetch16();

	return v | fetch16() << 16;
}

static u32
fetchsz(sz)
	int sz;
{
	switch (sz) {
	case 0:
		return fetch8();
	case 1:
		return fetch16();
	}
	return fetch32();
}

/* ---- stack ---- */

static u32
getsp()
{
	return cpu->s[S_SS].big ? cpu->r[R_SP] : cpu->r[R_SP] & 0xffff;
}

static void
setsp(v)
	u32 v;
{
	if (cpu->s[S_SS].big)
		cpu->r[R_SP] = v;
	else
		cpu->r[R_SP] = (cpu->r[R_SP] & ~0xffff) | (v & 0xffff);
}

static u32
spmask()
{
	return cpu->s[S_SS].big ? 0xffffffff : 0xffff;
}

static void
push(sz, v)
	int sz;
	u32 v;
{
	u32 sp = (getsp() - (sz == 1 ? 2 : 4)) & spmask();

	wr(S_SS, sp, sz, v);
	setsp(sp);
}

static u32
pop(sz)
	int sz;
{
	u32 sp = getsp(), v = rd(S_SS, sp, sz);

	setsp(sp + (sz == 1 ? 2 : 4));
	return v;
}

void
x86_push16(c, v)
	struct x86 *c;
	u32 v;
{
	struct x86 *o = cpu;
	u8 *om = mem;
	u32 sp;

	cpu = c;
	mem = c->mem;
	sp = (getsp() - 2) & spmask();
	WR16(mem, c->s[S_SS].base + sp, v);
	setsp(sp);
	cpu = o;
	mem = om;
}

u32
x86_pop16(c)
	struct x86 *c;
{
	struct x86 *o = cpu;
	u8 *om = mem;
	u32 sp, v;

	cpu = c;
	mem = c->mem;
	sp = getsp();
	v = RD16(mem, c->s[S_SS].base + sp);
	setsp(sp + 2);
	cpu = o;
	mem = om;
	return v;
}

/* ---- segments ---- */

static int
loadseg(sr, sel)
	int sr;
	u32 sel;
{
	struct seg *s = &cpu->s[sr];
	struct desc *d;

	sel &= 0xffff;
	if (!cpu->prot) {
		s->sel = sel;
		s->base = sel << 4;
		if (sr == S_CS)
			s->acc = D_P | D_S | D_CODE | D_R | D_A;
		return 0;
	}
	if ((sel & ~3) == 0) {
		if (sr == S_SS || sr == S_CS)
			return X_GP;
		s->sel = sel;
		s->acc = 0;
		s->base = 0;
		s->limit = 0;
		return 0;
	}
	if (!(sel & 4) || SELIX(sel) >= LDTSIZE)
		return X_GP;
	d = &cpu->ldt[SELIX(sel)];
	if (!(d->d_acc & D_S))
		return X_GP;
	if (sr == S_CS) {
		if (!(d->d_acc & D_CODE))
			return X_GP;
	} else if (sr == S_SS) {
		if ((d->d_acc & (D_CODE | D_W)) != D_W)
			return X_GP;
	} else if ((d->d_acc & (D_CODE | D_R)) == D_CODE)
		return X_GP;
	if (!(d->d_acc & D_P))
		return sr == S_SS ? X_SS : X_NP;
	s->sel = sel;
	s->base = d->d_base;
	s->limit = d->d_limit;
	s->acc = d->d_acc;
	s->big = 0;
	return 0;
}

int
x86_loadseg(c, sr, sel)
	struct x86 *c;
	int sr;
	u32 sel;
{
	struct x86 *o = cpu;
	int r;

	cpu = c;
	r = loadseg(sr, sel);
	cpu = o;
	return r;
}

static void
setseg(sr, sel)
	int sr;
	u32 sel;
{
	int x = loadseg(sr, sel);

	if (x)
		fault(x, sel & 0xfffc);
}

u32
x86_lin(c, sel, off)
	struct x86 *c;
	u32 sel, off;
{
	struct desc *d;

	if (!c->prot)
		return (sel << 4) + off;
	if (!(sel & 4) || SELIX(sel) >= LDTSIZE)
		return ~0;
	d = &c->ldt[SELIX(sel)];
	if (!(d->d_acc & D_P) || off > d->d_limit)
		return ~0;
	return d->d_base + off;
}

/* ---- ModRM ---- */

static int
defseg(sr)
	int sr;
{
	return cpu->ovseg >= 0 ? cpu->ovseg : sr;
}

static void
modrm()
{
	u32 b = fetch8(), off = 0, base, index;
	int sr = S_DS, sib, sc;

	m_mod = b >> 6;
	m_reg = b >> 3 & 7;
	m_rm = b & 7;
	if (m_mod == 3)
		return;
	if (!cpu->a32) {
		switch (m_rm) {
		case 0: off = cpu->r[R_BX] + cpu->r[R_SI]; break;
		case 1: off = cpu->r[R_BX] + cpu->r[R_DI]; break;
		case 2: off = cpu->r[R_BP] + cpu->r[R_SI]; sr = S_SS; break;
		case 3: off = cpu->r[R_BP] + cpu->r[R_DI]; sr = S_SS; break;
		case 4: off = cpu->r[R_SI]; break;
		case 5: off = cpu->r[R_DI]; break;
		case 6:
			if (m_mod == 0)
				off = fetch16();
			else {
				off = cpu->r[R_BP];
				sr = S_SS;
			}
			break;
		case 7: off = cpu->r[R_BX]; break;
		}
		if (m_mod == 1)
			off += (s32)(signed char)fetch8();
		else if (m_mod == 2)
			off += fetch16();
		m_off = off & 0xffff;
	} else {
		if (m_rm == 4) {
			sib = fetch8();
			sc = sib >> 6;
			index = sib >> 3 & 7;
			base = sib & 7;
			/* no index: the 386 scales the base instead */
			off = index == 4 ? 0 : cpu->r[index] << sc;
			if (base == 5 && m_mod == 0)
				off += fetch32();
			else {
				off += index == 4 ? cpu->r[base] << sc : cpu->r[base];
				if (base == R_SP || base == R_BP)
					sr = S_SS;
			}
		} else if (m_rm == 5 && m_mod == 0)
			off = fetch32();
		else {
			off = cpu->r[m_rm];
			if (m_rm == R_BP)
				sr = S_SS;
		}
		if (m_mod == 1)
			off += (s32)(signed char)fetch8();
		else if (m_mod == 2)
			off += fetch32();
		m_off = off;
	}
	m_seg = defseg(sr);
}

static u32
getrm(sz)
	int sz;
{
	if (m_mod == 3)
		return getreg(sz, m_rm);
	return rd(m_seg, m_off, sz);
}

static void
setrm(sz, v)
	int sz;
	u32 v;
{
	if (m_mod == 3)
		setreg(sz, m_rm, v);
	else
		wr(m_seg, m_off, sz, v);
}

/* the memory operand's offset plus n, in the address size */
static u32
offplus(n)
	u32 n;
{
	return cpu->a32 ? m_off + n : (m_off + n) & 0xffff;
}

/* ---- ALU ---- */

static u32
alu(op, sz, d, s)
	int op, sz;
	u32 d, s;
{
	u32 r, m = szmask[sz], c;

	d &= m;
	s &= m;
	switch (op) {
	case 0:		/* add */
		r = (d + s) & m;
		LAZY(LF_ADD, sz, d, s, r);
		break;
	case 1:		/* or */
		r = d | s;
		LAZY(LF_LOGIC, sz, d, s, r);
		break;
	case 2:		/* adc */
		c = getcf();
		r = (d + s + c) & m;
		LAZY(LF_ADD, sz, d, s, r);
		break;
	case 3:		/* sbb */
		c = getcf();
		r = (d - s - c) & m;
		LAZY(LF_SUB, sz, d, s, r);
		break;
	case 4:		/* and */
		r = d & s;
		LAZY(LF_LOGIC, sz, d, s, r);
		break;
	case 5:		/* sub */
	case 7:		/* cmp */
		r = (d - s) & m;
		LAZY(LF_SUB, sz, d, s, r);
		break;
	default:	/* xor */
		r = d ^ s;
		LAZY(LF_LOGIC, sz, d, s, r);
		break;
	}
	return r;
}

static u32
incdec(sz, v, dec)
	int sz, dec;
	u32 v;
{
	u32 r, m = szmask[sz];

	eager();
	v &= m;
	r = (dec ? v - 1 : v + 1) & m;
	LAZY(dec ? LF_DEC : LF_INC, sz, v, 1, r);
	return r;
}

/* shifts and rotates, group 2: flags made here */
static u32
shift(op, sz, v, cnt)
	int op, sz;
	u32 v;
	int cnt;
{
	u32 m = szmask[sz], t = sztop[sz], r = v & m, f;
	int bits = 8 << sz, n, cf;

	cnt &= 31;
	if (cnt == 0)
		return r;
	f = FLAGS();
	v &= m;
	switch (op) {
	case 0:		/* rol */
		n = cnt % bits;
		r = n ? ((v << n) | (v >> (bits - n))) & m : v;
		cf = r & 1;
		f = (f & ~(F_CF | F_OF)) | cf | (((r & t) ? 1 : 0) ^ cf ? F_OF : 0);
		SETF(f);
		return r;
	case 1:		/* ror */
		n = cnt % bits;
		r = n ? ((v >> n) | (v << (bits - n))) & m : v;
		cf = (r & t) != 0;
		f = (f & ~(F_CF | F_OF)) | cf | (((r ^ (r << 1)) & t) ? F_OF : 0);
		SETF(f);
		return r;
		/* right shifts and rotates: OF is the top two bits' difference */
	case 2:		/* rcl */
		n = sz == 2 ? cnt : cnt % (bits + 1);
		cf = f & F_CF;
		r = v;
		while (n--) {
			int out = (r & t) != 0;

			r = ((r << 1) | cf) & m;
			cf = out;
		}
		f = (f & ~(F_CF | F_OF)) | cf | (((r & t) ? 1 : 0) ^ cf ? F_OF : 0);
		SETF(f);
		return r;
	case 3:		/* rcr */
		n = sz == 2 ? cnt : cnt % (bits + 1);
		cf = f & F_CF;
		r = v;
		while (n--) {
			int out = r & 1;

			r = (r >> 1) | (cf ? t : 0);
			cf = out;
		}
		f = (f & ~(F_CF | F_OF)) | cf | (((r ^ (r << 1)) & t) ? F_OF : 0);
		SETF(f);
		return r;
	case 4:		/* shl */
	case 6:
		if (cnt <= bits)
			cf = cnt <= 32 ? (v << (cnt - 1) & t) != 0 : 0;
		else
			cf = 0;
		r = cnt >= 32 ? 0 : (v << cnt) & m;
		f &= ~(F_CF | F_OF | F_AF);
		f |= cf | ((((r & t) != 0) ^ cf) ? F_OF : 0);
		break;
	case 5:		/* shr */
		cf = cnt <= 32 ? (v >> (cnt - 1)) & 1 : 0;
		r = cnt >= 32 ? 0 : v >> cnt;
		f &= ~(F_CF | F_OF | F_AF);
		f |= cf | (((r ^ (r << 1)) & t) ? F_OF : 0);
		break;
	default:	/* sar */
		{
			s32 sv = (v & t) ? (s32)(v | ~m) : (s32)v;

			cf = (sv >> (cnt - 1)) & 1;
			r = (u32)(sv >> cnt) & m;
		}
		f &= ~(F_CF | F_OF | F_AF);
		f |= cf;
		break;
	}
	SETF(f);
	szp(sz, r);
	return r;
}

/* shld/shrd */
static u32
dshift(sz, d, s, cnt, left)
	int sz, cnt, left;
	u32 d, s;
{
	u32 m = szmask[sz], t = sztop[sz], r, f;
	int cf, i;

	cnt &= 31;
	if (cnt == 0)
		return d & m;
	d &= m;
	s &= m;
	f = FLAGS();
	r = d;
	cf = f & F_CF;
	/* 16-bit counts above 16 shift the source in again, as the 386 does */
	for (i = 0; i < cnt; i++) {
		u32 sb;

		if (left) {
			cf = (r & t) != 0;
			sb = (s & t) != 0;
			r = ((r << 1) | sb) & m;
			s = ((s << 1) | (d & t ? 1 : 0)) & m;
			d = (d << 1) & m;
		} else {
			cf = r & 1;
			sb = s & 1;
			r = (r >> 1) | (sb ? t : 0);
			s = (s >> 1) | (d & 1 ? t : 0);
			d >>= 1;
		}
	}
	f &= ~(F_CF | F_OF | F_AF);
	f |= cf;
	SETF(f);
	szp(sz, r);
	return r;
}

/* ---- control transfer ---- */

static void
jumpnear(ip)
	u32 ip;
{
	if (!cpu->o32)
		ip &= 0xffff;
	if (ip > cpu->s[S_CS].limit)
		fault(X_GP, 0);
	cpu->eip = ip;
}

static void
jumpfar(sel, ip)
	u32 sel, ip;
{
	struct seg save;
	int x;

	save = cpu->s[S_CS];
	x = loadseg(S_CS, sel);
	if (x)
		fault(x, sel & 0xfffc);
	if (ip > cpu->s[S_CS].limit) {
		cpu->s[S_CS] = save;
		fault(X_GP, 0);
	}
	cpu->eip = ip;
}

/* real-mode interrupt through the vector table */
static void
rmint(n, ip)
	int n;
	u32 ip;
{
	u32 f = FLAGS(), v = n * 4;

	if (v + 3 > 0x3ff)	/* IDT limit */
		fault(X_GP, n * 8 + 2);
	push(1, f);
	push(1, cpu->s[S_CS].sel);
	push(1, ip);
	cpu->fl &= ~(F_IF | F_TF | F_RF);
	loadseg(S_CS, RD16(mem, v + 2));
	cpu->eip = RD16(mem, v);
}

/* INT n and the traps: the host first in protected mode */
static void
softint(n)
	int n;
{
	if (cpu->prot) {
		if (cpu->intr && (*cpu->intr)(cpu, n))
			return;
		fault(X_GP, n * 8 + 2);
	}
	rmint(n, cpu->eip);
}

/* ---- string instructions ---- */

static u32
sreg(i)
	int i;
{
	return cpu->a32 ? cpu->r[i] : cpu->r[i] & 0xffff;
}

static void
sadv(i, d)
	int i;
	int d;
{
	if (cpu->a32)
		cpu->r[i] += d;
	else
		cpu->r[i] = (cpu->r[i] & ~0xffff) | ((cpu->r[i] + d) & 0xffff);
}

static u32
scount()
{
	return cpu->a32 ? cpu->r[R_CX] : cpu->r[R_CX] & 0xffff;
}

static void
sdec()
{
	sadv(R_CX, -1);
}

/* op: a4 movs, a6 cmps, aa stos, ac lods, ae scas, 6c ins, 6e outs */
static void
string(op, sz)
	int op, sz;
{
	int d = (cpu->fl & F_DF) ? -(1 << sz) : 1 << sz;
	int src = defseg(S_DS);
	u32 a, b;

	for (;;) {
		if (cpu->rep && scount() == 0)
			return;
		switch (op) {
		case 0xa4:
			a = rd(src, sreg(R_SI), sz);
			wr(S_ES, sreg(R_DI), sz, a);
			sadv(R_SI, d);
			sadv(R_DI, d);
			break;
		case 0xa6:
			a = rd(src, sreg(R_SI), sz);
			b = rd(S_ES, sreg(R_DI), sz);
			alu(7, sz, a, b);
			sadv(R_SI, d);
			sadv(R_DI, d);
			break;
		case 0xaa:
			wr(S_ES, sreg(R_DI), sz, getreg(sz, R_AX));
			sadv(R_DI, d);
			break;
		case 0xac:
			a = rd(src, sreg(R_SI), sz);
			setreg(sz, R_AX, a);
			sadv(R_SI, d);
			break;
		case 0xae:
			b = rd(S_ES, sreg(R_DI), sz);
			alu(7, sz, getreg(sz, R_AX), b);
			sadv(R_DI, d);
			break;
		case 0x6c:
			lin(S_ES, sreg(R_DI), 1 << sz, 1);
			a = 0xffffffff;
			if (cpu->io)
				(*cpu->io)(cpu, cpu->r[R_DX] & 0xffff, sz, 0, &a);
			wr(S_ES, sreg(R_DI), sz, a);
			sadv(R_DI, d);
			break;
		case 0x6e:
			a = rd(src, sreg(R_SI), sz);
			if (cpu->io)
				(*cpu->io)(cpu, cpu->r[R_DX] & 0xffff, sz, 1, &a);
			sadv(R_SI, d);
			break;
		}
		if (!cpu->rep)
			return;
		sdec();
		if (op == 0xa6 || op == 0xae) {
			if (cpu->rep == 0xf3 && !getzf())
				return;
			if (cpu->rep == 0xf2 && getzf())
				return;
		}
	}
}

/* ---- multiply and divide ---- */

static void
mulflags(on)
	int on;
{
	u32 f = FLAGS() & ~(F_CF | F_OF);

	SETF(f | (on ? F_CF | F_OF : 0));
}

static void
grp3(sz)
	int sz;
{
	u32 v, a, r, m = szmask[sz];
	unsigned long long p, q, dv;
	long long sp, sq, sdv, sa;

	switch (m_reg) {
	case 0:
	case 1:		/* test */
		v = getrm(sz);
		alu(4, sz, v, fetchsz(sz));
		return;
	case 2:		/* not */
		v = getrm(sz);
		setrm(sz, ~v);
		return;
	case 3:		/* neg */
		v = getrm(sz);
		r = alu(5, sz, 0, v);
		setrm(sz, r);
		return;
	case 4:		/* mul */
		v = getrm(sz);
		switch (sz) {
		case 0:
			r = (cpu->r[R_AX] & 0xff) * v;
			setreg(1, R_AX, r);
			mulflags(r >> 8);
			szp(0, r);
			return;
		case 1:
			r = (cpu->r[R_AX] & 0xffff) * v;
			setreg(1, R_AX, r);
			setreg(1, R_DX, r >> 16);
			mulflags(r >> 16);
			szp(1, r);
			return;
		}
		p = (unsigned long long)cpu->r[R_AX] * v;
		cpu->r[R_AX] = p;
		cpu->r[R_DX] = p >> 32;
		mulflags(cpu->r[R_DX] != 0);
		szp(2, (u32)p);
		return;
	case 5:		/* imul */
		v = getrm(sz);
		switch (sz) {
		case 0:
			r = (s32)(signed char)cpu->r[R_AX] * (s32)(signed char)v;
			setreg(1, R_AX, r);
			mulflags((s32)(signed char)r != (s32)(short)r);
			szp(0, r);
			return;
		case 1:
			r = (s32)(short)cpu->r[R_AX] * (s32)(short)v;
			setreg(1, R_AX, r);
			setreg(1, R_DX, r >> 16);
			mulflags((s32)(short)r != (s32)r);
			szp(1, r);
			return;
		}
		sp = (long long)(s32)cpu->r[R_AX] * (s32)v;
		cpu->r[R_AX] = sp;
		cpu->r[R_DX] = (unsigned long long)sp >> 32;
		mulflags((long long)(s32)sp != sp);
		szp(2, (u32)sp);
		return;
	case 6:		/* div */
		v = getrm(sz);
		if (v == 0)
			fault(X_DE, 0);
		switch (sz) {
		case 0:
			a = cpu->r[R_AX] & 0xffff;
			if (a / v > 0xff)
				fault(X_DE, 0);
			setreg(1, R_AX, (a % v) << 8 | a / v);
			return;
		case 1:
			a = (cpu->r[R_DX] & 0xffff) << 16 | (cpu->r[R_AX] & 0xffff);
			if (a / v > 0xffff)
				fault(X_DE, 0);
			setreg(1, R_AX, a / v);
			setreg(1, R_DX, a % v);
			return;
		}
		dv = (unsigned long long)cpu->r[R_DX] << 32 | cpu->r[R_AX];
		q = dv / v;
		if (q > 0xffffffffULL)
			fault(X_DE, 0);
		cpu->r[R_AX] = q;
		cpu->r[R_DX] = dv % v;
		return;
	default:	/* idiv */
		v = getrm(sz);
		if ((v & m) == 0)
			fault(X_DE, 0);
		switch (sz) {
		case 0:
			sa = (short)cpu->r[R_AX];
			sdv = (signed char)v;
			sq = sa / sdv;
			if (sq > 127 || sq < -128)
				fault(X_DE, 0);
			setreg(1, R_AX, ((u32)(sa % sdv) & 0xff) << 8 | ((u32)sq & 0xff));
			return;
		case 1:
			sa = (s32)((cpu->r[R_DX] & 0xffff) << 16 | (cpu->r[R_AX] & 0xffff));
			sdv = (short)v;
			sq = sa / sdv;
			if (sq > 32767 || sq < -32768)
				fault(X_DE, 0);
			setreg(1, R_AX, (u32)sq);
			setreg(1, R_DX, (u32)(sa % sdv));
			return;
		}
		sa = (long long)((unsigned long long)cpu->r[R_DX] << 32 | cpu->r[R_AX]);
		sdv = (s32)v;
		if (sa == (-0x7fffffffffffffffLL - 1) && sdv == -1)
			fault(X_DE, 0);
		sq = sa / sdv;
		if (sq > 0x7fffffffLL || sq < -0x80000000LL)
			fault(X_DE, 0);
		cpu->r[R_AX] = (u32)sq;
		cpu->r[R_DX] = (u32)(sa % sdv);
		return;
	}
}

/* imul r, r/m, imm (and 0F AF with imm the register) */
static u32
imul3(sz, a, b)
	int sz;
	u32 a, b;
{
	long long p;
	u32 r;

	if (sz == 1) {
		p = (long long)(short)a * (short)b;
		r = (u32)p & 0xffff;
		mulflags((long long)(short)r != p);
	} else {
		p = (long long)(s32)a * (s32)b;
		r = (u32)p;
		mulflags((long long)(s32)r != p);
	}
	szp(sz, r);
	return r;
}

/* ---- BCD ---- */

static void
bcd(op)
	int op;
{
	u32 f = FLAGS(), al = cpu->r[R_AX] & 0xff, v, old = al;
	int cf = f & F_CF, af = (f & F_AF) != 0;

	switch (op) {
	case 0x27:	/* daa */
		f &= ~(F_CF | F_AF);
		if ((al & 15) > 9 || af) {
			if (al + 6 > 0xff)
				f |= F_CF;
			al += 6;
			f |= F_AF;
		}
		if (old > 0x99 || cf) {
			al += 0x60;
			f |= F_CF;
		}
		al &= 0xff;
		f &= ~F_OF;
		f |= ((old ^ al) & al & 0x80) ? F_OF : 0;
		SETF(f);
		setreg(0, R_AX, al);
		szp(0, al);
		break;
	case 0x2f:	/* das */
		f &= ~(F_CF | F_AF);
		if ((al & 15) > 9 || af) {
			if (al < 6)
				f |= F_CF;
			al -= 6;
			f |= F_AF;
		}
		if (old > 0x99 || cf) {
			al -= 0x60;
			f |= F_CF;
		}
		al &= 0xff;
		f &= ~F_OF;
		f |= ((old ^ al) & old & 0x80) ? F_OF : 0;
		SETF(f);
		setreg(0, R_AX, al);
		szp(0, al);
		break;
	case 0x37:	/* aaa: AX + 0x106 on the 286 and later */
	case 0x3f:	/* aas */
		v = cpu->r[R_AX] & 0xffff;
		if ((al & 15) > 9 || af) {
			if (op == 0x37)
				v += 0x106;
			else
				v = (v - 6 - 0x100) & 0xffff;
			f |= F_AF | F_CF;
		} else
			f &= ~(F_AF | F_CF);
		SETF(f);
		szp(0, al);
		setreg(1, R_AX, (v & 0xff00) | (v & 15));
		break;
	}
}

/* ---- descriptor instructions (0F 00, 02, 03) ---- */

static struct desc *
seldesc(sel)
	u32 sel;
{
	struct desc *d;

	if ((sel & ~3) == 0 || !(sel & 4) || SELIX(sel) >= LDTSIZE)
		return 0;
	d = &cpu->ldt[SELIX(sel)];
	return d->d_acc ? d : 0;
}

/* ---- the 0F page ---- */

static void
op0f()
{
	int op = fetch8(), sz = cpu->o32 ? 2 : 1, x;
	u32 v, a, sel, off, r;
	struct desc *d;

	switch (op) {
	case 0x00:		/* group 6 */
		if (!cpu->prot)
			fault(X_UD, 0);
		modrm();
		switch (m_reg) {
		case 0:		/* sldt */
		case 1:		/* str */
			setrm(m_mod == 3 ? sz : 1, 0);
			return;
		case 4:		/* verr */
		case 5:		/* verw */
			d = seldesc(getrm(1));
			if (m_reg == 4)
				x = d && (d->d_acc & D_S) && (!(d->d_acc & D_CODE) || (d->d_acc & D_R));
			else
				x = d && (d->d_acc & (D_S | D_CODE | D_W)) == (D_S | D_W);
			setfl(F_ZF, x);
			return;
		}
		fault(X_GP, 0);
	case 0x01:		/* group 7 */
		modrm();
		switch (m_reg) {
		case 4:		/* smsw */
			setrm(m_mod == 3 ? sz : 1, cpu->cr0);
			return;
		case 0:
		case 1:		/* sgdt, sidt: as a 386 with tables at 0 */
			if (m_mod == 3)
				fault(X_UD, 0);
			wr(m_seg, m_off, 1, 0x3ff);
			wr(m_seg, offplus(2), 2, cpu->o32 ? 0 : 0xff000000);
			return;
		}
		fault(cpu->prot ? X_GP : X_UD, 0);
	case 0x02:		/* lar */
	case 0x03:		/* lsl */
		if (!cpu->prot)
			fault(X_UD, 0);
		modrm();
		d = seldesc(getrm(1));
		if (!d) {
			setfl(F_ZF, 0);
			return;
		}
		if (op == 2)
			setreg(sz, m_reg, (u32)(d->d_acc | 0x60) << 8);
		else
			setreg(sz, m_reg, d->d_limit);
		setfl(F_ZF, 1);
		return;
	case 0x06:		/* clts */
		return;
	case THUNK_OP:
		if (!cpu->prot || !cpu->thunk)
			fault(X_UD, 0);
		v = fetch16();
		if (v == x86_retthunk) {
			cpu->stop = 1;
			return;
		}
		(*cpu->thunk)(cpu, v);
		return;
	case 0xa0: push(sz, cpu->s[S_FS].sel); return;
	case 0xa8: push(sz, cpu->s[S_GS].sel); return;
	case 0xa1:
	case 0xa9:
		v = rd(S_SS, getsp(), 1);
		setseg(op == 0xa1 ? S_FS : S_GS, v);
		setsp(getsp() + (sz == 1 ? 2 : 4));
		return;
	case 0xa3: case 0xab: case 0xb3: case 0xbb:	/* bt bts btr btc r/m, r */
	case 0xba:					/* group 8, imm */
		modrm();
		if (op == 0xba) {
			x = m_reg & 3;
			if (m_reg < 4)
				fault(X_UD, 0);
			a = fetch8() & (sz == 1 ? 15 : 31);
			off = 0;
		} else {
			x = (op >> 3) & 3;
			a = getreg(sz, m_reg);
			if (m_mod != 3) {
				/* the bit offset is signed and reaches outside the operand */
				s32 sa = sz == 1 ? (s32)(short)a : (s32)a;

				off = (u32)((sa >> (sz == 1 ? 4 : 5)) * (sz == 1 ? 2 : 4));
			} else
				off = 0;
			a &= sz == 1 ? 15 : 31;
		}
		if (m_mod == 3)
			v = getreg(sz, m_rm);
		else {
			m_off = offplus(off);
			v = rd(m_seg, m_off, sz);
		}
		setfl(F_CF, (v >> a) & 1);
		switch (x) {
		case 0: return;			/* bt */
		case 1: v |= 1u << a; break;	/* bts */
		case 2: v &= ~(1u << a); break;	/* btr */
		case 3: v ^= 1u << a; break;	/* btc */
		}
		setrm(sz, v);
		return;
	case 0xa4: case 0xa5: case 0xac: case 0xad:	/* shld, shrd */
		modrm();
		v = getrm(sz);
		x = (op & 1) ? cpu->r[R_CX] & 0xff : fetch8();
		r = dshift(sz, v, getreg(sz, m_reg), x, op < 0xa8);
		setrm(sz, r);
		return;
	case 0xaf:		/* imul r, r/m */
		modrm();
		v = getrm(sz);
		setreg(sz, m_reg, imul3(sz, getreg(sz, m_reg), v));
		return;
	case 0xb2: case 0xb4: case 0xb5:	/* lss, lfs, lgs */
		modrm();
		if (m_mod == 3)
			fault(X_UD, 0);
		off = rd(m_seg, m_off, sz);
		sel = rd(m_seg, offplus(sz == 1 ? 2 : 4), 1);
		setseg(op == 0xb2 ? S_SS : op == 0xb4 ? S_FS : S_GS, sel);
		setreg(sz, m_reg, off);
		return;
	case 0xb6: case 0xb7:	/* movzx */
		modrm();
		v = getrm(op & 1);
		setreg(sz, m_reg, v);
		return;
	case 0xbe: case 0xbf:	/* movsx */
		modrm();
		v = getrm(op & 1);
		v = (op & 1) ? (u32)(s32)(short)v : (u32)(s32)(signed char)v;
		setreg(sz, m_reg, v);
		return;
	case 0xbc: case 0xbd:	/* bsf, bsr */
		modrm();
		v = getrm(sz) & szmask[sz];
		if (v == 0) {
			setfl(F_ZF, 1);
			return;
		}
		if (op == 0xbc)
			for (a = 0; !(v >> a & 1); a++)
				;
		else
			for (a = sz == 1 ? 15 : 31; !(v >> a & 1); a--)
				;
		setreg(sz, m_reg, a);
		setfl(F_ZF, 0);
		return;
	case 0xc8: case 0xc9: case 0xca: case 0xcb:
	case 0xcc: case 0xcd: case 0xce: case 0xcf:	/* bswap (486) */
		v = cpu->r[op & 7];
		cpu->r[op & 7] = v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24;
		return;
	}
	if (op >= 0x80 && op <= 0x8f) {		/* jcc near */
		v = cpu->o32 ? fetch32() : fetch16();
		if (!cpu->o32)
			v = (u32)(s32)(short)v;
		if (cond(op & 15))
			jumpnear(cpu->eip + v);
		return;
	}
	if (op >= 0x90 && op <= 0x9f) {		/* setcc */
		modrm();
		setrm(0, cond(op & 15));
		return;
	}
	fault(X_UD, 0);
}

/* ---- one instruction ---- */

/* flags from popf or iret: the low word; IOPL stays in protected mode */
static u32
newflags(v)
	u32 v;
{
	u32 a = FLAGS();

	v = (a & 0xffff0000) | (v & 0x7fd7) | 2;
	if (cpu->prot)
		v = (v & ~F_IOPL) | (a & F_IOPL);
	return v;
}

/* LOCK goes only with these, and a memory destination */
static int
lockable(op)
	int op;
{
	u32 a = cpu->s[S_CS].base + cpu->eip;
	int m, r;

	if (a + 2 >= cpu->memsize)
		return 0;
	m = mem[a];
	if (op == 0x0f) {
		op = 0x100 | m;
		m = mem[a + 1];
	}
	r = m >> 3 & 7;
	if ((m >> 6) == 3)
		return 0;
	switch (op) {
	case 0x00: case 0x01: case 0x08: case 0x09: case 0x10: case 0x11:
	case 0x18: case 0x19: case 0x20: case 0x21: case 0x28: case 0x29:
	case 0x30: case 0x31: case 0x86: case 0x87:
	case 0x1ab: case 0x1b3: case 0x1bb:
		return 1;
	case 0x80: case 0x81: case 0x82: case 0x83:
		return r != 7;
	case 0xf6: case 0xf7:
		return r == 2 || r == 3;
	case 0xfe: case 0xff:
		return r < 2;
	case 0x1ba:
		return r >= 5;
	}
	return 0;
}

static void
exec()
{
	int op, sz, x, i, lock = 0;
	u32 v, a, b, sel, off;

	cpu->ip0 = cpu->eip;
	cpu->sp0 = cpu->r[R_SP];
	cpu->ovseg = -1;
	cpu->o32 = cpu->a32 = cpu->s[S_CS].big;
	cpu->rep = 0;
	for (;;) {
		op = fetch8();
		switch (op) {
		case 0x26: cpu->ovseg = S_ES; continue;
		case 0x2e: cpu->ovseg = S_CS; continue;
		case 0x36: cpu->ovseg = S_SS; continue;
		case 0x3e: cpu->ovseg = S_DS; continue;
		case 0x64: cpu->ovseg = S_FS; continue;
		case 0x65: cpu->ovseg = S_GS; continue;
		case 0x66: cpu->o32 = !cpu->s[S_CS].big; continue;
		case 0x67: cpu->a32 = !cpu->s[S_CS].big; continue;
		case 0xf0: lock = 1; continue;
		case 0xf2:
		case 0xf3: cpu->rep = op; continue;
		}
		break;
	}
	cpu->icount++;
	sz = cpu->o32 ? 2 : 1;
	if (lock && !lockable(op))
		fault(X_UD, 0);

	if (op < 0x40 && (op & 7) < 6) {	/* ALU */
		x = op >> 3;
		switch (op & 7) {
		case 0:
		case 1:
			modrm();
			i = (op & 1) ? sz : 0;
			v = alu(x, i, getrm(i), getreg(i, m_reg));
			if (x != 7)
				setrm(i, v);
			return;
		case 2:
		case 3:
			modrm();
			i = (op & 1) ? sz : 0;
			v = alu(x, i, getreg(i, m_reg), getrm(i));
			if (x != 7)
				setreg(i, m_reg, v);
			return;
		case 4:
			v = alu(x, 0, cpu->r[R_AX], fetch8());
			if (x != 7)
				setreg(0, R_AX, v);
			return;
		case 5:
			v = alu(x, sz, cpu->r[R_AX], fetchsz(sz));
			if (x != 7)
				setreg(sz, R_AX, v);
			return;
		}
	}
	switch (op) {
	case 0x06: push(sz, cpu->s[S_ES].sel); return;
	case 0x0e: push(sz, cpu->s[S_CS].sel); return;
	case 0x16: push(sz, cpu->s[S_SS].sel); return;
	case 0x1e: push(sz, cpu->s[S_DS].sel); return;
	case 0x07:
	case 0x17:
	case 0x1f:
		v = rd(S_SS, getsp(), 1);
		setseg(op >> 3, v);
		setsp(getsp() + (sz == 1 ? 2 : 4));
		return;
	case 0x0f:
		op0f();
		return;
	case 0x27: case 0x2f: case 0x37: case 0x3f:
		bcd(op);
		return;
	case 0x40: case 0x41: case 0x42: case 0x43:
	case 0x44: case 0x45: case 0x46: case 0x47:
		setreg(sz, op & 7, incdec(sz, cpu->r[op & 7], 0));
		return;
	case 0x48: case 0x49: case 0x4a: case 0x4b:
	case 0x4c: case 0x4d: case 0x4e: case 0x4f:
		setreg(sz, op & 7, incdec(sz, cpu->r[op & 7], 1));
		return;
	case 0x50: case 0x51: case 0x52: case 0x53:
	case 0x54: case 0x55: case 0x56: case 0x57:
		push(sz, cpu->r[op & 7]);
		return;
	case 0x58: case 0x59: case 0x5a: case 0x5b:
	case 0x5c: case 0x5d: case 0x5e: case 0x5f:
		v = pop(sz);
		setreg(sz, op & 7, v);
		return;
	case 0x60:		/* pusha */
		a = getsp();
		for (i = 0; i < 8; i++)
			push(sz, i == R_SP ? a : cpu->r[i]);
		return;
	case 0x61:		/* popa */
		b = 0;
		for (i = 7; i >= 0; i--) {
			v = pop(sz);
			if (i != R_SP)
				setreg(sz, i, v);
			else
				b = v;
		}
		/* popad on a 16-bit stack: ESP's top half from the image */
		if (sz == 2 && !cpu->s[S_SS].big)
			cpu->r[R_SP] = (b & 0xffff0000) | (cpu->r[R_SP] & 0xffff);
		return;
	case 0x62:		/* bound */
		modrm();
		if (m_mod == 3)
			fault(X_UD, 0);
		v = getreg(sz, m_reg);
		a = rd(m_seg, m_off, sz);
		b = rd(m_seg, offplus(sz == 1 ? 2 : 4), sz);
		if (sz == 1) {
			if ((short)v < (short)a || (short)v > (short)b)
				fault(X_BR, 0);
		} else if ((s32)v < (s32)a || (s32)v > (s32)b)
			fault(X_BR, 0);
		return;
	case 0x63:		/* arpl */
		if (!cpu->prot)
			fault(X_UD, 0);
		modrm();
		v = getrm(1);
		a = getreg(1, m_reg);
		if ((v & 3) < (a & 3)) {
			setrm(1, (v & ~3) | (a & 3));
			setfl(F_ZF, 1);
		} else
			setfl(F_ZF, 0);
		return;
	case 0x68:
		push(sz, fetchsz(sz));
		return;
	case 0x6a:
		push(sz, (u32)(s32)(signed char)fetch8());
		return;
	case 0x69:
	case 0x6b:
		modrm();
		v = getrm(sz);
		a = op == 0x6b ? (u32)(s32)(signed char)fetch8() : fetchsz(sz);
		setreg(sz, m_reg, imul3(sz, v, a));
		return;
	case 0x6c: string(op, 0); return;
	case 0x6d: string(0x6c, sz); return;
	case 0x6e: string(op, 0); return;
	case 0x6f: string(0x6e, sz); return;
	case 0x80:
	case 0x81:
	case 0x82:
	case 0x83:
		i = op == 0x81 || op == 0x83 ? sz : 0;
		modrm();
		v = getrm(i);
		a = op == 0x81 ? fetchsz(sz) : (u32)(s32)(signed char)fetch8();
		v = alu(m_reg, i, v, a);
		if (m_reg != 7)
			setrm(i, v);
		return;
	case 0x84:
	case 0x85:
		i = op & 1 ? sz : 0;
		modrm();
		alu(4, i, getrm(i), getreg(i, m_reg));
		return;
	case 0x86:
	case 0x87:
		i = op & 1 ? sz : 0;
		modrm();
		v = getrm(i);
		a = getreg(i, m_reg);
		setrm(i, a);
		setreg(i, m_reg, v);
		return;
	case 0x88: modrm(); setrm(0, getreg(0, m_reg)); return;
	case 0x89: modrm(); setrm(sz, getreg(sz, m_reg)); return;
	case 0x8a: modrm(); setreg(0, m_reg, getrm(0)); return;
	case 0x8b: modrm(); setreg(sz, m_reg, getrm(sz)); return;
	case 0x8c:
		modrm();
		if (m_reg > 5)
			fault(X_UD, 0);
		v = cpu->s[m_reg].sel;
		if (m_mod == 3)
			setreg(sz, m_rm, v);
		else
			setrm(1, v);
		return;
	case 0x8d:
		modrm();
		if (m_mod == 3)
			fault(X_UD, 0);
		setreg(sz, m_reg, m_off);
		return;
	case 0x8e:
		modrm();
		if (m_reg == S_CS || m_reg > 5)
			fault(X_UD, 0);
		setseg(m_reg, getrm(1));
		return;
	case 0x8f:
		/* the address uses SP after the pop */
		a = cpu->r[R_SP];
		if ((cpu->s[S_CS].base + cpu->eip) < cpu->memsize && (mem[cpu->s[S_CS].base + cpu->eip] >> 3 & 7))
			fault(X_UD, 0);
		v = rd(S_SS, getsp(), sz);
		setsp(getsp() + (sz == 1 ? 2 : 4));
		modrm();
		if (m_mod == 3) {
			setreg(sz, m_rm, v);
			return;
		}
		cpu->r[R_SP] = a;
		setrm(sz, v);
		setsp(getsp() + (sz == 1 ? 2 : 4));
		return;
	case 0x90:
		return;
	case 0x91: case 0x92: case 0x93:
	case 0x94: case 0x95: case 0x96: case 0x97:
		v = getreg(sz, op & 7);
		setreg(sz, op & 7, getreg(sz, R_AX));
		setreg(sz, R_AX, v);
		return;
	case 0x98:
		if (sz == 1)
			setreg(1, R_AX, (u32)(s32)(signed char)cpu->r[R_AX]);
		else
			cpu->r[R_AX] = (u32)(s32)(short)cpu->r[R_AX];
		return;
	case 0x99:
		if (sz == 1)
			setreg(1, R_DX, cpu->r[R_AX] & 0x8000 ? 0xffff : 0);
		else
			cpu->r[R_DX] = cpu->r[R_AX] & 0x80000000 ? 0xffffffff : 0;
		return;
	case 0x9a:		/* call far */
		off = fetchsz(sz);
		sel = fetch16();
		a = cpu->s[S_CS].sel;
		b = cpu->eip;
		v = getsp();
		lin(S_SS, (v - 2 * (sz == 1 ? 2 : 4)) & spmask(), 2 * (sz == 1 ? 2 : 4), 1);
		jumpfar(sel, off);
		push(sz, a);
		push(sz, b);
		return;
	case 0x9b:		/* wait */
		return;
	case 0x9c:
		v = FLAGS() & 0xffff;
		push(sz, v);
		return;
	case 0x9d:
		v = pop(sz);
		SETF(newflags(v));
		return;
	case 0x9e:		/* sahf */
		v = FLAGS();
		v = (v & ~0xd5) | (cpu->r[R_AX] >> 8 & 0xd5);
		SETF(v);
		return;
	case 0x9f:		/* lahf */
		v = FLAGS();
		setreg(0, 4, (v & 0xd7) | 2);
		return;
	case 0xa0: case 0xa1: case 0xa2: case 0xa3:
		off = cpu->a32 ? fetch32() : fetch16();
		i = op & 1 ? sz : 0;
		if (op & 2)
			wr(defseg(S_DS), off, i, getreg(i, R_AX));
		else
			setreg(i, R_AX, rd(defseg(S_DS), off, i));
		return;
	case 0xa4: case 0xa6: case 0xaa: case 0xac: case 0xae:
		string(op, 0);
		return;
	case 0xa5: case 0xa7: case 0xab: case 0xad: case 0xaf:
		string(op - 1, sz);
		return;
	case 0xa8:
		alu(4, 0, cpu->r[R_AX], fetch8());
		return;
	case 0xa9:
		alu(4, sz, cpu->r[R_AX], fetchsz(sz));
		return;
	case 0xb0: case 0xb1: case 0xb2: case 0xb3:
	case 0xb4: case 0xb5: case 0xb6: case 0xb7:
		setreg(0, op & 7, fetch8());
		return;
	case 0xb8: case 0xb9: case 0xba: case 0xbb:
	case 0xbc: case 0xbd: case 0xbe: case 0xbf:
		setreg(sz, op & 7, fetchsz(sz));
		return;
	case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3:
		i = op & 1 ? sz : 0;
		modrm();
		v = getrm(i);
		if (op < 0xd0)
			x = fetch8();
		else if (op < 0xd2)
			x = 1;
		else
			x = cpu->r[R_CX] & 0xff;
		v = shift(m_reg, i, v, x);
		setrm(i, v);
		return;
	case 0xc2:
	case 0xc3:
		a = op == 0xc2 ? fetch16() : 0;
		v = rd(S_SS, getsp(), sz);
		jumpnear(v);
		setsp(getsp() + (sz == 1 ? 2 : 4) + a);
		return;
	case 0xc4:
	case 0xc5:
		modrm();
		if (m_mod == 3)
			fault(X_UD, 0);
		off = rd(m_seg, m_off, sz);
		sel = rd(m_seg, offplus(sz == 1 ? 2 : 4), 1);
		setseg(op == 0xc4 ? S_ES : S_DS, sel);
		setreg(sz, m_reg, off);
		return;
	case 0xc6:
	case 0xc7:
		modrm();
		if (m_reg)
			fault(X_UD, 0);
		i = op & 1 ? sz : 0;
		setrm(i, fetchsz(i));
		return;
	case 0xc8:		/* enter */
		a = fetch16();
		x = fetch8() & 31;
		{
			u32 fp, bp = cpu->r[R_BP], sp, n = sz == 1 ? 2 : 4;

			push(sz, bp);
			fp = getsp();
			sp = fp;
			for (i = 1; i < x; i++) {
				bp = cpu->s[S_SS].big ? bp - n : (bp - n) & 0xffff;
				v = rd(S_SS, bp, sz);
				sp = (sp - n) & spmask();
				wr(S_SS, sp, sz, v);
			}
			if (x) {
				sp = (sp - n) & spmask();
				wr(S_SS, sp, sz, fp);
			}
			setsp(sp);
			setreg(sz, R_BP, fp);
			setsp(getsp() - a);
		}
		return;
	case 0xc9:		/* leave */
		v = cpu->s[S_SS].big ? cpu->r[R_BP] : cpu->r[R_BP] & 0xffff;
		a = rd(S_SS, v, sz);
		setsp(v + (sz == 1 ? 2 : 4));
		setreg(sz, R_BP, a);
		return;
	case 0xca:
	case 0xcb:		/* retf */
		a = op == 0xca ? fetch16() : 0;
		v = getsp();
		off = rd(S_SS, v, sz);
		sel = rd(S_SS, (v + (sz == 1 ? 2 : 4)) & spmask(), 1);
		jumpfar(sel, off);
		setsp(v + 2 * (sz == 1 ? 2 : 4) + a);
		return;
	case 0xcc:
		softint(3);
		return;
	case 0xcd:
		x = fetch8();
		softint(x);
		return;
	case 0xce:
		if (getof())
			softint(4);
		return;
	case 0xcf:		/* iret */
		v = getsp();
		off = rd(S_SS, v, sz);
		sel = rd(S_SS, (v + (sz == 1 ? 2 : 4)) & spmask(), 1);
		a = rd(S_SS, (v + 2 * (sz == 1 ? 2 : 4)) & spmask(), sz);
		jumpfar(sel, off);
		setsp(v + 3 * (sz == 1 ? 2 : 4));
		SETF(newflags(a));
		return;
	case 0x70: case 0x71: case 0x72: case 0x73:
	case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7a: case 0x7b:
	case 0x7c: case 0x7d: case 0x7e: case 0x7f:
		v = (u32)(s32)(signed char)fetch8();
		if (cond(op & 15))
			jumpnear(cpu->eip + v);
		return;
	case 0xd4:		/* aam */
		x = fetch8();
		if (x == 0)
			fault(X_DE, 0);
		v = cpu->r[R_AX] & 0xff;
		setreg(1, R_AX, (v / x) << 8 | (v % x));
		eager();
		cpu->fl &= ~(F_CF | F_OF | F_AF);
		szp(0, v % x);
		return;
	case 0xd5:		/* aad */
		x = fetch8();
		v = ((cpu->r[R_AX] >> 8 & 0xff) * x + (cpu->r[R_AX] & 0xff)) & 0xff;
		alu(0, 0, (cpu->r[R_AX] >> 8 & 0xff) * x, cpu->r[R_AX] & 0xff);
		setreg(1, R_AX, v);
		return;
	case 0xd6:		/* salc */
		setreg(0, R_AX, getcf() ? 0xff : 0);
		return;
	case 0xd7:		/* xlat */
		a = cpu->a32 ? cpu->r[R_BX] + (cpu->r[R_AX] & 0xff) : (cpu->r[R_BX] + (cpu->r[R_AX] & 0xff)) & 0xffff;
		setreg(0, R_AX, rd(defseg(S_DS), a, 0));
		return;
	case 0xd8: case 0xd9: case 0xda: case 0xdb:
	case 0xdc: case 0xdd: case 0xde: case 0xdf:
		if (!cpu->fpu || (cpu->cr0 & 0xc))
			fault(X_NM, 0);
		modrm();
		x87_exec(cpu, op, m_mod, m_reg, m_rm, m_seg, m_off);
		return;
	case 0xe0: case 0xe1: case 0xe2: case 0xe3:	/* loopnz, loopz, loop, jcxz */
		v = (u32)(s32)(signed char)fetch8();
		if (op == 0xe3)
			x = (cpu->a32 ? cpu->r[R_CX] : cpu->r[R_CX] & 0xffff) == 0;
		else {
			sadv(R_CX, -1);
			x = (cpu->a32 ? cpu->r[R_CX] : cpu->r[R_CX] & 0xffff) != 0;
			if (op == 0xe0)
				x = x && !getzf();
			else if (op == 0xe1)
				x = x && getzf();
		}
		if (x)
			jumpnear(cpu->eip + v);
		return;
	case 0xe4: case 0xe5: case 0xec: case 0xed:	/* in */
		a = op < 0xe8 ? fetch8() : cpu->r[R_DX] & 0xffff;
		i = op & 1 ? sz : 0;
		v = 0xffffffff;
		if (cpu->io)
			(*cpu->io)(cpu, a, i, 0, &v);
		setreg(i, R_AX, v);
		return;
	case 0xe6: case 0xe7: case 0xee: case 0xef:	/* out */
		a = op < 0xe8 ? fetch8() : cpu->r[R_DX] & 0xffff;
		i = op & 1 ? sz : 0;
		v = getreg(i, R_AX);
		if (cpu->io)
			(*cpu->io)(cpu, a, i, 1, &v);
		return;
	case 0xe8:
		v = cpu->o32 ? fetch32() : (u32)(s32)(short)fetch16();
		a = cpu->eip;
		b = (a + v) & (cpu->o32 ? 0xffffffff : 0xffff);
		if (b > cpu->s[S_CS].limit)
			fault(X_GP, 0);
		push(sz, a);
		cpu->eip = b;
		return;
	case 0xe9:
		v = cpu->o32 ? fetch32() : (u32)(s32)(short)fetch16();
		jumpnear(cpu->eip + v);
		return;
	case 0xea:
		off = fetchsz(sz);
		sel = fetch16();
		jumpfar(sel, off);
		return;
	case 0xeb:
		v = (u32)(s32)(signed char)fetch8();
		jumpnear(cpu->eip + v);
		return;
	case 0xf4:
		cpu->halted = 1;
		cpu->stop = 1;
		return;
	case 0xf5:
		setfl(F_CF, !getcf());
		return;
	case 0xf6:
		modrm();
		grp3(0);
		return;
	case 0xf7:
		modrm();
		grp3(sz);
		return;
	case 0xf8: setfl(F_CF, 0); return;
	case 0xf9: setfl(F_CF, 1); return;
	case 0xfa: cpu->fl &= ~F_IF; return;
	case 0xfb: cpu->fl |= F_IF; return;
	case 0xfc: cpu->fl &= ~F_DF; return;
	case 0xfd: cpu->fl |= F_DF; return;
	case 0xfe:
		modrm();
		if (m_reg > 1)
			fault(X_UD, 0);
		setrm(0, incdec(0, getrm(0), m_reg));
		return;
	case 0xff:
		modrm();
		switch (m_reg) {
		case 0:
		case 1:
			setrm(sz, incdec(sz, getrm(sz), m_reg));
			return;
		case 2:		/* call near */
			v = getrm(sz);
			a = cpu->eip;
			b = v & (cpu->o32 ? 0xffffffff : 0xffff);
			if (b > cpu->s[S_CS].limit)
				fault(X_GP, 0);
			push(sz, a);
			cpu->eip = b;
			return;
		case 3:		/* call far */
		case 5:		/* jmp far */
			if (m_mod == 3)
				fault(X_UD, 0);
			off = rd(m_seg, m_off, sz);
			sel = rd(m_seg, offplus(sz == 1 ? 2 : 4), 1);
			if (m_reg == 5) {
				jumpfar(sel, off);
				return;
			}
			a = cpu->s[S_CS].sel;
			b = cpu->eip;
			v = getsp();
			lin(S_SS, (v - 2 * (sz == 1 ? 2 : 4)) & spmask(), 2 * (sz == 1 ? 2 : 4), 1);
			jumpfar(sel, off);
			push(sz, a);
			push(sz, b);
			return;
		case 4:
			jumpnear(getrm(sz));
			return;
		case 6:
			v = getrm(sz);
			push(sz, v);
			return;
		}
		fault(X_UD, 0);
	}
	fault(X_UD, 0);
}

/* ---- running ---- */

void
x86_init(c, m, size, ldt)
	struct x86 *c;
	u8 *m;
	u32 size;
	struct desc *ldt;
{
	int i, j, p;

	for (i = 0; i < 256; i++) {
		for (p = 0, j = 0; j < 8; j++)
			p ^= i >> j & 1;
		parity[i] = p ? 0 : F_PF;
	}
	memset((char *)c, 0, sizeof *c);
	c->mem = m;
	c->memsize = size;
	c->ldt = ldt;
	c->fl = 2;
	for (i = 0; i < 6; i++) {
		c->s[i].limit = 0xffff;
		c->s[i].acc = D_P | D_S | D_W | D_A;
	}
	c->s[S_CS].acc = D_P | D_S | D_CODE | D_R | D_A;
	if (c->fpu)
		x87_init(c);
}

/*
 * Deliver the exception in hand: IP is back at the instruction.  In
 * real mode through the vector table; else the host decides (0: stop).
 */
static int
deliver()
{
	int n = xnum, e = xerr;

	cpu->eip = cpu->ip0;
	cpu->r[R_SP] = cpu->sp0;
	if (!cpu->prot) {
		rmint(n, cpu->eip);
		return 1;
	}
	if (cpu->fault && (*cpu->fault)(cpu, n, e))
		return 1;
	cpu->stop = 1;
	return 0;
}

static int indeliver;

/* the run loop: exceptions come back here and are delivered */
static void
loop(c, one)
	struct x86 *c;
	int one;
{
	jmp_buf jb, *ojb = trapjb;
	struct x86 *o = cpu;
	u8 *om = mem;
	int r;

	cpu = c;
	mem = c->mem;
	trapjb = &jb;
	indeliver = 0;
	for (;;) {
		if (setjmp(jb) == 0) {
			if (indeliver) {
				r = deliver();
				indeliver = 0;
				if (!r || one)
					break;
			}
			if (one)
				exec();
			else
				while (!c->stop)
					exec();
			break;
		}
		cpu = c;
		mem = c->mem;
		if (indeliver) {	/* a fault while delivering one: stop */
			indeliver = 0;
			c->stop = 1;
			break;
		}
		indeliver = 1;
	}
	trapjb = ojb;
	cpu = o;
	mem = om;
}

int
x86_step(c)
	struct x86 *c;
{
	c->stop = 0;
	loop(c, 1);
	return c->stop;
}

void
x86_run(c)
	struct x86 *c;
{
	c->stop = 0;
	loop(c, 0);
}

/*
 * Far call into guest code from the host, with what the callee needs
 * already pushed: the return goes to the return thunk at the host's
 * thunk selector (retsel:retoff), which stops this level's run.
 */
static u32 retsel, retoff;

void
x86_setret(sel, off)
	u32 sel, off;
{
	retsel = sel;
	retoff = off;
}

int
x86_call(c, sel, off)
	struct x86 *c;
	u32 sel, off;
{
	u32 cs = c->s[S_CS].sel, ip = c->eip;
	int x;

	x86_push16(c, retsel);
	x86_push16(c, retoff);
	if ((x = x86_loadseg(c, S_CS, sel)) != 0)
		return -1;
	c->eip = off;
	c->depth++;
	x86_run(c);
	c->depth--;
	if (c->halted)
		return -1;
	c->stop = 0;
	x86_loadseg(c, S_CS, cs);
	c->eip = ip;
	return 0;
}
