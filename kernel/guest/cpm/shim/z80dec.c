/* 8080/Z80 decoder shared by execution and instruction inspection.
 * Prefix groups retain their complete lengths even when execution is
 * unsupported. Guest instruction fetch wraps at 16 bits. */

#include "z80.h"

/* Fetch the k'th byte of the instruction at pc.  The (z16) cast is the
 * wrap: an instruction straddling 0xFFFF continues at 0. */
static int fb(m, pc, k)
char *m;
z16 pc;
int k;
{
	return (m[(z16)(pc + k)] & 0xff);
}

/* Sign-extend a byte without assuming char's signedness -- MWC and the
 * host disagree about it and this file must not. */
static z16 sx(b)
int b;
{
	b &= 0xff;
	return ((z16)(b & 0x80 ? (0xff00 | b) : b));
}

static z16 iw(m, pc, k)
char *m;
z16 pc;
int k;
{
	return ((z16)(fb(m, pc, k) | (fb(m, pc, k + 1) << 8)));
}

/*
 * Base-map lengths, transcribed from the encoding rather than from a
 * table someone else built.  Three rules cover all 256:
 *
 *   3 bytes -- an instruction carrying a 16-bit address or datum:
 *		LXI (x1), the four direct loads/stores (22 2A 32 3A),
 *		JMP/Jcc (xx010 xx011), CALL/Ccc (xx100 CD).
 *   2 bytes -- an 8-bit immediate: MVI (xx110), the ALU-immediate eight
 *		(11xxx110), IN, OUT, and the Z80 relative branches
 *		(10 18 20 28 30 38), whose operand is a displacement.
 *   1 byte  -- everything else.
 *
 * Held as data: a compare chain over 26 labels costs more than the
 * decode it serves.
 */
static z8 blen[256] = {
	1, 3, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,	/* 00 */
	2, 3, 1, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 2, 1,	/* 10 */
	2, 3, 3, 1, 1, 1, 2, 1, 2, 1, 3, 1, 1, 1, 2, 1,	/* 20 */
	2, 3, 3, 1, 1, 1, 2, 1, 2, 1, 3, 1, 1, 1, 2, 1,	/* 30 */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 40 */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 50 */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 60 */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 70 */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 80 */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 90 */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* A0 */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* B0 */
	1, 1, 3, 3, 3, 1, 2, 1, 1, 1, 3, 1, 3, 3, 2, 1,	/* C0 */
	1, 1, 3, 2, 3, 1, 2, 1, 1, 1, 3, 2, 3, 1, 2, 1,	/* D0 */
	1, 1, 3, 1, 3, 1, 2, 1, 1, 1, 3, 1, 3, 1, 2, 1,	/* E0 */
	1, 1, 3, 1, 3, 1, 2, 1, 1, 1, 3, 1, 3, 1, 2, 1	/* F0 */
};

static int baselen(op)
int op;
{
	return ((int)blen[op & 0xff]);
}

/*
 * The rest of the base map, the same way: outside the four prefixes an
 * opcode byte fixes the operation and its operand fields on its own, so
 * a decode is four loads and an operand fetch.  Read them as a 16-wide
 * grid -- the row comment is the opcode of the first entry on the line.
 *
 * bx is the operand field the opcode carries in the place its group
 * puts it: the register pair for LXI/DAD/INX/DCX/PUSH/POP, the
 * destination register for MOV/MVI/INR/DCR, the operation for the ALU
 * eight and the rotates, the condition for Jcc/Ccc/Rcc and JR, the
 * vector for RST.  by is the source register, 6 meaning the byte at
 * (HL).  The prefix rows are never reached; they are filled so the
 * grid stays a grid.
 */
static z8 bop[256] = {
	Z_NOP, Z_LXI, Z_STAX, Z_INX, Z_INR, Z_DCR, Z_LDRI, Z_ROT,	/* 00 */
	Z_EXAF, Z_DAD, Z_LDAX, Z_DCX, Z_INR, Z_DCR, Z_LDRI, Z_ROT,	/* 08 */
	Z_DJNZ, Z_LXI, Z_STAX, Z_INX, Z_INR, Z_DCR, Z_LDRI, Z_ROT,	/* 10 */
	Z_JR, Z_DAD, Z_LDAX, Z_DCX, Z_INR, Z_DCR, Z_LDRI, Z_ROT,	/* 18 */
	Z_JR, Z_LXI, Z_SHLD, Z_INX, Z_INR, Z_DCR, Z_LDRI, Z_DAA,	/* 20 */
	Z_JR, Z_DAD, Z_LHLD, Z_DCX, Z_INR, Z_DCR, Z_LDRI, Z_CMA,	/* 28 */
	Z_JR, Z_LXI, Z_STA, Z_INX, Z_INR, Z_DCR, Z_LDRI, Z_STC,		/* 30 */
	Z_JR, Z_DAD, Z_LDA, Z_DCX, Z_INR, Z_DCR, Z_LDRI, Z_CMC,		/* 38 */
	Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR,	/* 40 */
	Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR,	/* 48 */
	Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR,	/* 50 */
	Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR,	/* 58 */
	Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR,	/* 60 */
	Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR,	/* 68 */
	Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_HLT, Z_LDRR,	/* 70 */
	Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR, Z_LDRR,	/* 78 */
	Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU,		/* 80 */
	Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU,		/* 88 */
	Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU,		/* 90 */
	Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU,		/* 98 */
	Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU,		/* A0 */
	Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU,		/* A8 */
	Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU,		/* B0 */
	Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU, Z_ALU,		/* B8 */
	Z_RCC, Z_POP, Z_JCC, Z_JMP, Z_CCC, Z_PUSH, Z_ALU, Z_RST,	/* C0 */
	Z_RCC, Z_RET, Z_JCC, Z_CB, Z_CCC, Z_CALL, Z_ALU, Z_RST,		/* C8 */
	Z_RCC, Z_POP, Z_JCC, Z_OUT, Z_CCC, Z_PUSH, Z_ALU, Z_RST,	/* D0 */
	Z_RCC, Z_EXX, Z_JCC, Z_IN, Z_CCC, Z_IX, Z_ALU, Z_RST,		/* D8 */
	Z_RCC, Z_POP, Z_JCC, Z_XTHL, Z_CCC, Z_PUSH, Z_ALU, Z_RST,	/* E0 */
	Z_RCC, Z_PCHL, Z_JCC, Z_XCHG, Z_CCC, Z_ED, Z_ALU, Z_RST,	/* E8 */
	Z_RCC, Z_POP, Z_JCC, Z_DI, Z_CCC, Z_PUSH, Z_ALU, Z_RST,		/* F0 */
	Z_RCC, Z_SPHL, Z_JCC, Z_EI, Z_CCC, Z_IX, Z_ALU, Z_RST		/* F8 */
};

static z8 bx[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,	/* 00 */
	0, 1, 1, 1, 2, 2, 2, 2, 4, 1, 1, 1, 3, 3, 3, 3,	/* 10 */
	0, 2, 0, 2, 4, 4, 4, 0, 1, 2, 0, 2, 5, 5, 5, 0,	/* 20 */
	2, 3, 0, 3, 6, 6, 6, 0, 3, 3, 0, 3, 7, 7, 7, 0,	/* 30 */
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,	/* 40 */
	2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,	/* 50 */
	4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5,	/* 60 */
	6, 6, 6, 6, 6, 6, 0, 6, 7, 7, 7, 7, 7, 7, 7, 7,	/* 70 */
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,	/* 80 */
	2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,	/* 90 */
	4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5,	/* A0 */
	6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7,	/* B0 */
	0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1,	/* C0 */
	2, 1, 2, 0, 2, 1, 2, 2, 3, 0, 3, 0, 3, 0, 3, 3,	/* D0 */
	4, 2, 4, 0, 4, 2, 4, 4, 5, 0, 5, 0, 5, 0, 5, 5,	/* E0 */
	6, 3, 6, 0, 6, 3, 6, 6, 7, 0, 7, 0, 7, 0, 7, 7	/* F0 */
};

static z8 by[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 00 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 10 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 20 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 30 */
	0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7,	/* 40 */
	0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7,	/* 50 */
	0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7,	/* 60 */
	0, 1, 2, 3, 4, 5, 0, 7, 0, 1, 2, 3, 4, 5, 6, 7,	/* 70 */
	0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7,	/* 80 */
	0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7,	/* 90 */
	0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7,	/* A0 */
	0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7,	/* B0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* C0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* D0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* E0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0	/* F0 */
};

static z8 bfl[256] = {
	0, ZF_IMM, ZF_MEM, 0, 0, 0, ZF_IMM, 0,			/* 00 */
	0, 0, ZF_MEM, 0, 0, 0, ZF_IMM, 0,			/* 08 */
	0, ZF_IMM, ZF_MEM, 0, 0, 0, ZF_IMM, 0,			/* 10 */
	0, 0, ZF_MEM, 0, 0, 0, ZF_IMM, 0,			/* 18 */
	0, ZF_IMM, ZF_IMM|ZF_MEM|ZF_ADDR, 0, 0, 0, ZF_IMM, 0,	/* 20 */
	0, 0, ZF_IMM|ZF_MEM|ZF_ADDR, 0, 0, 0, ZF_IMM, 0,	/* 28 */
	0, ZF_IMM, ZF_IMM|ZF_MEM|ZF_ADDR, 0,
		ZF_MEM, ZF_MEM, ZF_IMM|ZF_MEM, 0,		/* 30 */
	0, 0, ZF_IMM|ZF_MEM|ZF_ADDR, 0, 0, 0, ZF_IMM, 0,	/* 38 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 40 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 48 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 50 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 58 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 60 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 68 */
	ZF_MEM, ZF_MEM, ZF_MEM, ZF_MEM, ZF_MEM, ZF_MEM, 0, ZF_MEM, /* 70 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 78 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 80 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 88 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 90 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* 98 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* A0 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* A8 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* B0 */
	0, 0, 0, 0, 0, 0, ZF_MEM, 0,				/* B8 */
	0, 0, ZF_ADDR, ZF_ADDR, ZF_ADDR, 0, ZF_IMM, 0,		/* C0 */
	0, 0, ZF_ADDR, ZF_PFX, ZF_ADDR, ZF_ADDR, ZF_IMM, 0,	/* C8 */
	0, 0, ZF_ADDR, 0, ZF_ADDR, 0, ZF_IMM, 0,		/* D0 */
	0, 0, ZF_ADDR, 0, ZF_ADDR, ZF_PFX, ZF_IMM, 0,		/* D8 */
	0, 0, ZF_ADDR, ZF_MEM, ZF_ADDR, 0, ZF_IMM, 0,		/* E0 */
	0, 0, ZF_ADDR, 0, ZF_ADDR, ZF_PFX, ZF_IMM, 0,		/* E8 */
	0, 0, ZF_ADDR, 0, ZF_ADDR, 0, ZF_IMM, 0,		/* F0 */
	0, 0, ZF_ADDR, 0, ZF_ADDR, ZF_PFX, ZF_IMM, 0		/* F8 */
};

/*
 * Does this base opcode name the memory byte at (HL)?
 *
 * It matters twice.  Here, because under a DD/FD prefix every one of
 * these grows a signed displacement byte -- (HL) becomes (IX+d) -- and
 * that is the ONLY thing the prefix does to the length.  And in the
 * executor, because r = 6 is a memory reference and not a register.
 *
 * The set is: INR/DCR/MVI with r = 6 (34 35 36), MOV r,M (x1xxx110 with
 * the source field 6: 46 4E 56 5E 66 6E 7E), MOV M,r (70-77 less 76,
 * which is HLT and is not a move at all), and the ALU eight against M
 * (86 8E 96 9E A6 AE B6 BE).
 */
static int usesm(op)
int op;
{
	if (op == 0x34 || op == 0x35 || op == 0x36)
		return (1);
	if (op >= 0x70 && op <= 0x77)
		return (op != 0x76);		/* 76 is HLT		*/
	if (op >= 0x40 && op <= 0x7f)
		return ((op & 7) == 6);
	if (op >= 0x80 && op <= 0xbf)
		return ((op & 7) == 6);
	return (0);
}

/*
 * ED-group lengths.  Two bytes, except the four LD (nn),dd and four
 * LD dd,(nn) forms, which carry a 16-bit address: ED 43/53/63/73 store
 * and ED 4B/5B/6B/7B load.  Everything else in the group -- the block
 * moves, the 16-bit ADC/SBC, NEG, RETN, IM, the I/R transfers, and every
 * undefined second byte -- is two.
 */
static int edlen(sub)
int sub;
{
	switch (sub) {
	case 0x43: case 0x4b: case 0x53: case 0x5b:
	case 0x63: case 0x6b: case 0x73: case 0x7b:
		return (4);
	}
	return (2);
}

/*
 * z80dec -- decode the instruction at m[pc] into *in.
 *
 * Returns the instruction length in bytes.  On a byte that cannot begin
 * an instruction, sets in->op = Z_BAD and returns the length consumed so
 * far (at least 1) so that a linear sweep can step past it -- a sweep
 * that stopped dead at the first table byte would report one finding and
 * hide the rest.
 *
 * On the base map there is no such byte: all 256 decode.  Z_BAD is
 * reachable only through the ED escape, where our own hook occupies a
 * second byte the hardware leaves undefined.
 */
int z80dec(m, pc, in)
char *m;
z16 pc;
struct z80in *in;
{
	register int op, sub;
	register int n, fl;

	op = m[pc] & 0xff;

	/* ---- the base map: the grid answers everything but the operand,
	 * and ZF_PFX marks the four bytes that are not on it. */
	fl = (int)bfl[op];
	if (!(fl & ZF_PFX)) {
		n = (int)blen[op];
		in->op = bop[op];
		in->x = bx[op];
		in->y = by[op];
		in->fl = (z8)fl;
		in->len = (z8)n;
		in->pfx = 0;
		in->sub = 0;
		in->disp = 0;
		if (n == 1)
			in->imm = 0;
		else if (n == 3)
			in->imm = iw(m, pc, 1);
		else if (in->op == Z_JR || in->op == Z_DJNZ)
			in->imm = (z16)(sx(fb(m, pc, 1)) + pc + 2);
		else
			in->imm = (z16)fb(m, pc, 1);
		return (n);
	}

	in->op = Z_BAD;
	in->fl = 0;
	in->x = 0;
	in->y = 0;
	in->pfx = 0;
	in->sub = 0;
	in->imm = 0;
	in->disp = 0;

	/* ---- the four prefixes: each is a different opcode space with a
	 * different length rule. */
	if (op == 0xcb) {
		/* CB is the one prefix group with a SINGLE regular
		 * encoding: two bits of group, three of operation or bit
		 * number, three of register, always two bytes.  So the
		 * fields are pulled apart here rather than in the
		 * executor, on the rule the head comment states -- one
		 * decode serves three consumers, and a second copy of
		 * this shift-and-mask in z80exec.c would be a second
		 * thing to get wrong.
		 *
		 *	.x   0 rotate/shift, 1 BIT, 2 RES, 3 SET
		 *	.y   the register, 6 meaning the byte at (HL)
		 *	.imm the rotate operation 0..7, or the bit 0..7 */
		sub = fb(m, pc, 1);
		in->op = Z_CB;
		in->fl |= ZF_PFX;
		in->pfx = (z8)op;
		in->sub = (z8)sub;
		in->x = (z8)((sub >> 6) & 3);
		in->y = (z8)(sub & 7);
		in->imm = (z16)((sub >> 3) & 7);
		if (in->y == R_M)
			in->fl |= ZF_MEM;
		n = 2;
		goto done;
	}
	if (op == 0xed) {
		sub = fb(m, pc, 1);
		in->fl |= ZF_PFX;
		in->pfx = (z8)op;
		in->sub = (z8)sub;
		/* OUR escape.  ED FE is undefined on the part -- the Z80
		 * executes an undefined ED pair as two NOPs and no
		 * assembler emits one -- so it can be spent on the shim's
		 * BDOS and BIOS entry points without taking an encoding
		 * away from the guest.  An 8080-undefined byte would not
		 * have survived contact with the Z80; see HOOK_* in
		 * z80.h. */
		if (sub == 0xfe) {
			in->op = Z_HOOK;
			in->x = (z8)fb(m, pc, 2);
			n = 3;
			if (in->x > HOOK_MAX)
				in->op = Z_BAD;
			goto done;
		}
		in->op = Z_ED;
		n = edlen(sub);
		/* The eight LD (nn),dd and LD dd,(nn) forms are the only
		 * members of the group that carry an operand, and they
		 * carry the same one the base map's SHLD/LHLD do.  .x is
		 * the register pair in the ordinary rp encoding, so
		 * ED 7B is `LD SP,(nn)' with .x = P_SP and needs no
		 * special case in the executor. */
		if (n == 4) {
			in->x = (z8)((sub >> 4) & 3);
			in->imm = iw(m, pc, 2);
			in->fl |= ZF_IMM | ZF_ADDR | ZF_MEM;
		}
		goto done;
	}
	if (op == 0xdd || op == 0xfd) {
		sub = fb(m, pc, 1);
		in->fl |= ZF_PFX;
		in->pfx = (z8)op;
		in->sub = (z8)sub;
		if (sub == 0xdd || sub == 0xfd || sub == 0xed) {
			/* A prefix followed by another prefix is DISCARDED
			 * by the hardware: DD DD 21 nn nn is `LD IX,nn'
			 * with the first DD costing four T-states and
			 * nothing else, and DD ED B0 is a plain LDIR.  So
			 * the length here is ONE, not two -- the next
			 * decode has to start on the second prefix or the
			 * stream desynchronises.  This is the one length
			 * rule in the architecture that a table of
			 * "prefix plus opcode" gets wrong, and getting it
			 * wrong is silent: DD ED B0 read as a two-byte
			 * instruction leaves B0 to decode as ORA B. */
			in->op = Z_IX;
			n = 1;
			goto done;
		}
		if (sub == 0xcb) {
			/* DD CB d op: the displacement comes BEFORE the
			 * operation byte, which is the one length rule in
			 * the architecture that reads backwards. */
			in->op = Z_IXCB;
			in->disp = sx(fb(m, pc, 2));
			n = 4;
			goto done;
		}
		in->op = Z_IX;
		n = 1 + baselen(sub) + (usesm(sub) ? 1 : 0);
		if (usesm(sub))
			in->disp = sx(fb(m, pc, 2));
		goto done;
	}

done:
	if (n < 1)
		n = 1;
	in->len = (z8)n;
	return (n);
}
