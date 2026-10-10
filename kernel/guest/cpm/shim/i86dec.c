/* 8086 decoder shared by execution and instruction inspection.
 * Decode register-independent operands and lengths; effective addresses
 * are resolved by the executor. Guest instruction fetch wraps at 16 bits. */

#include "i86.h"

/* The architectural maximum instruction length is fifteen bytes, opcode
 * included (Intel SDM Vol. 2, general instruction format), so fourteen
 * is every prefix byte an instruction can carry. */
#define I86MAXPFX	14

/* Fetch the k'th byte of the instruction at cs:ip.  The (i16) cast is
 * the segment wrap: an instruction straddling 0xFFFF continues at 0. */
static int fb(cs, ip, k)
char *cs;
i16 ip;
int k;
{
	return (cs[(i16)(ip + k)] & 0xff);
}

/* Sign-extend a byte to 16 bits without assuming char's signedness --
 * MWC and the host disagree about it and this file must not. */
static i16 sx(b)
int b;
{
	b &= 0xff;
	return ((i16)(b & 0x80 ? (0xff00 | b) : b));
}

/* The eight prefix bytes: the four segment overrides, LOCK and its alias,
 * REPNE and REP.  One load says whether the prefix loop has anything to
 * do, which for almost every instruction it has not. */
static i8 ispfx[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 00 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 10 */
	0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0,	/* 20 */
	0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0,	/* 30 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 40 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 50 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 60 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 70 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 80 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 90 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* A0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* B0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* C0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* D0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* E0 */
	1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0	/* F0 */
};

/*
 * The base map, held as data.  Outside the prefixes an opcode byte fixes
 * the operation, its variant, and the shape of everything that follows,
 * so a decode is three loads and an operand fetch.  Read the tables as a
 * grid -- the row comment is the opcode of the first entry on the line.
 *
 * bop is the executable class and bx the variant it carries in .x: the
 * ALU operation, the condition for Jcc and LOOP, the segment slot for
 * the segment pushes and for LES/LDS, the string operation, the flag
 * action.  bform is what follows the opcode.
 *
 * 60-6F are filled as 70-7F: an 8086 ignores the top bits there and
 * executes the conditional branch, so both the length and the mnemonic
 * come out right without folding the opcode first.
 */
#define D_IMM	0x0007		/* mask: the operand the opcode carries	*/
#define D_IB	1		/* byte immediate			*/
#define D_IW	2		/* word immediate			*/
#define D_SB	3		/* sign-extended byte immediate		*/
#define D_J8	4		/* signed byte branch displacement	*/
#define D_JW	5		/* signed word branch displacement	*/
#define D_DA	6		/* direct address, into .disp		*/
#define D_FP	7		/* far pointer: offset, then segment	*/
#define D_M	0x0008		/* a mod r/m byte follows		*/
#define D_X	0x0010		/* .x is that byte's reg field		*/
#define D_3	0x0020		/* the operand is a register: mod = 3	*/
#define D_L	0x0040		/* ... named by the opcode's low three	*/
#define D_O	0x0080		/* the tail below has the rest		*/
#define D_I	0x0100		/* sets IN_IMM				*/
#define D_D	0x0200		/* sets IN_DIR				*/
#define D_W	0x0400		/* word operand				*/

static i8 bop[256] = {
	I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_PUSHSR, I_POPSR,	/* 00 */
	I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_PUSHSR, I_POPSR,	/* 08 */
	I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_PUSHSR, I_POPSR,	/* 10 */
	I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_PUSHSR, I_POPSR,	/* 18 */
	I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_BAD, I_DAA,	/* 20 */
	I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_BAD, I_DAS,	/* 28 */
	I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_BAD, I_AAA,	/* 30 */
	I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_ALU, I_BAD, I_AAS,	/* 38 */
	I_INC, I_INC, I_INC, I_INC, I_INC, I_INC, I_INC, I_INC,	/* 40 */
	I_DEC, I_DEC, I_DEC, I_DEC, I_DEC, I_DEC, I_DEC, I_DEC,	/* 48 */
	I_PUSH, I_PUSH, I_PUSH, I_PUSH, I_PUSH, I_PUSH, I_PUSH, I_PUSH,	/* 50 */
	I_POP, I_POP, I_POP, I_POP, I_POP, I_POP, I_POP, I_POP,	/* 58 */
	I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC,	/* 60 */
	I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC,	/* 68 */
	I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC,	/* 70 */
	I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC, I_JCC,	/* 78 */
	I_ALU, I_ALU, I_ALU, I_ALU, I_TEST, I_TEST, I_XCHG, I_XCHG,	/* 80 */
	I_MOV, I_MOV, I_MOV, I_MOV, I_MOVSR, I_LEA, I_MOVSR, I_POP,	/* 88 */
	I_NOP, I_XCHG, I_XCHG, I_XCHG, I_XCHG, I_XCHG, I_XCHG, I_XCHG,	/* 90 */
	I_CBW, I_CWD, I_CALLF, I_WAIT, I_PUSHF, I_POPF, I_SAHF, I_LAHF,	/* 98 */
	I_MOV, I_MOV, I_MOV, I_MOV, I_STRING, I_STRING, I_STRING,
		I_STRING,					/* A0 */
	I_TEST, I_TEST, I_STRING, I_STRING, I_STRING, I_STRING,
		I_STRING, I_STRING,				/* A8 */
	I_MOV, I_MOV, I_MOV, I_MOV, I_MOV, I_MOV, I_MOV, I_MOV,	/* B0 */
	I_MOV, I_MOV, I_MOV, I_MOV, I_MOV, I_MOV, I_MOV, I_MOV,	/* B8 */
	I_RET, I_RET, I_RET, I_RET, I_LXS, I_LXS, I_MOV, I_MOV,	/* C0 */
	I_RETF, I_RETF, I_RETF, I_RETF, I_INT, I_INT, I_INTO, I_IRET,	/* C8 */
	I_SHIFT, I_SHIFT, I_SHIFT, I_SHIFT, I_AAM, I_AAD, I_BAD, I_XLAT, /* D0 */
	I_ESC, I_ESC, I_ESC, I_ESC, I_ESC, I_ESC, I_ESC, I_ESC,	/* D8 */
	I_LOOP, I_LOOP, I_LOOP, I_LOOP, I_IO, I_IO, I_IO, I_IO,	/* E0 */
	I_CALL, I_JMP, I_JMPF, I_JMP, I_IO, I_IO, I_IO, I_IO,	/* E8 */
	I_BAD, I_BAD, I_BAD, I_BAD, I_HLT, I_FLAG, I_BAD, I_BAD,	/* F0 */
	I_FLAG, I_FLAG, I_FLAG, I_FLAG, I_FLAG, I_FLAG, I_BAD, I_BAD	/* F8 */
};

static i8 bx[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,		/* 00 */
	2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,		/* 10 */
	4, 4, 4, 4, 4, 4, 0, 0, 5, 5, 5, 5, 5, 5, 0, 0,		/* 20 */
	6, 6, 6, 6, 6, 6, 0, 0, 7, 7, 7, 7, 7, 7, 0, 0,		/* 30 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,		/* 40 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,		/* 50 */
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,	/* 60 */
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,	/* 70 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,		/* 80 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,		/* 90 */
	0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 3, 3, 4, 4,		/* A0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,		/* B0 */
	0, 0, 0, 0, S_ES, S_DS, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* C0 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,		/* D0 */
	0, 1, 2, 3, 0, 0, 1, 1, 0, 0, 0, 0, 2, 2, 3, 3,		/* E0 */
	0, 0, 0, 0, 0, 2, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0		/* F0 */
};

static i16 bform[256] = {
	D_M, D_M|D_W, D_M|D_D, D_M|D_W|D_D,				/* 00 */
	D_3|D_IB|D_I, D_3|D_IW|D_W|D_I, 0, 0,				/* 04 */
	D_M, D_M|D_W, D_M|D_D, D_M|D_W|D_D,				/* 08 */
	D_3|D_IB|D_I, D_3|D_IW|D_W|D_I, 0, 0,				/* 0C */
	D_M, D_M|D_W, D_M|D_D, D_M|D_W|D_D,				/* 10 */
	D_3|D_IB|D_I, D_3|D_IW|D_W|D_I, 0, 0,				/* 14 */
	D_M, D_M|D_W, D_M|D_D, D_M|D_W|D_D,				/* 18 */
	D_3|D_IB|D_I, D_3|D_IW|D_W|D_I, 0, 0,				/* 1C */
	D_M, D_M|D_W, D_M|D_D, D_M|D_W|D_D,				/* 20 */
	D_3|D_IB|D_I, D_3|D_IW|D_W|D_I, 0, 0,				/* 24 */
	D_M, D_M|D_W, D_M|D_D, D_M|D_W|D_D,				/* 28 */
	D_3|D_IB|D_I, D_3|D_IW|D_W|D_I, 0, 0,				/* 2C */
	D_M, D_M|D_W, D_M|D_D, D_M|D_W|D_D,				/* 30 */
	D_3|D_IB|D_I, D_3|D_IW|D_W|D_I, 0, 0,				/* 34 */
	D_M, D_M|D_W, D_M|D_D, D_M|D_W|D_D,				/* 38 */
	D_3|D_IB|D_I, D_3|D_IW|D_W|D_I, 0, 0,				/* 3C */
	D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,		/* 40 */
	D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,		/* 44 */
	D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,		/* 48 */
	D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,		/* 4C */
	D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,		/* 50 */
	D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,		/* 54 */
	D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,		/* 58 */
	D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,		/* 5C */
	D_J8, D_J8, D_J8, D_J8,					/* 60 */
	D_J8, D_J8, D_J8, D_J8,					/* 64 */
	D_J8, D_J8, D_J8, D_J8,					/* 68 */
	D_J8, D_J8, D_J8, D_J8,					/* 6C */
	D_J8, D_J8, D_J8, D_J8,					/* 70 */
	D_J8, D_J8, D_J8, D_J8,					/* 74 */
	D_J8, D_J8, D_J8, D_J8,					/* 78 */
	D_J8, D_J8, D_J8, D_J8,					/* 7C */
	D_M|D_X|D_IB|D_I, D_M|D_X|D_IW|D_W|D_I, D_M|D_X|D_IB|D_I,
		D_M|D_X|D_SB|D_W|D_I,					/* 80 */
	D_M, D_M|D_W, D_M, D_M|D_W,					/* 84 */
	D_M, D_M|D_W, D_M|D_D, D_M|D_W|D_D,				/* 88 */
	D_M|D_W|D_O, D_M|D_W, D_M|D_W|D_D|D_O, D_M|D_W,		/* 8C */
	0, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,			/* 90 */
	D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W, D_3|D_L|D_W,		/* 94 */
	0, 0, D_FP, 0,							/* 98 */
	0, 0, 0, 0,							/* 9C */
	D_DA|D_D, D_DA|D_W|D_D, D_DA, D_DA|D_W,			/* A0 */
	0, D_W, 0, D_W,						/* A4 */
	D_3|D_IB|D_I, D_3|D_IW|D_W|D_I, 0, D_W,			/* A8 */
	0, D_W, 0, D_W,						/* AC */
	D_3|D_L|D_IB|D_I, D_3|D_L|D_IB|D_I, D_3|D_L|D_IB|D_I,
		D_3|D_L|D_IB|D_I,					/* B0 */
	D_3|D_L|D_IB|D_I, D_3|D_L|D_IB|D_I, D_3|D_L|D_IB|D_I,
		D_3|D_L|D_IB|D_I,					/* B4 */
	D_3|D_L|D_IW|D_W|D_I, D_3|D_L|D_IW|D_W|D_I,
		D_3|D_L|D_IW|D_W|D_I, D_3|D_L|D_IW|D_W|D_I,		/* B8 */
	D_3|D_L|D_IW|D_W|D_I, D_3|D_L|D_IW|D_W|D_I,
		D_3|D_L|D_IW|D_W|D_I, D_3|D_L|D_IW|D_W|D_I,		/* BC */
	D_IW|D_I, 0, D_IW|D_I, 0,					/* C0 */
	D_M|D_W, D_M|D_W, D_M|D_IB|D_I, D_M|D_IW|D_W|D_I,		/* C4 */
	D_IW|D_I, 0, D_IW|D_I, 0,					/* C8 */
	D_O, D_IB, 0, 0,						/* CC */
	D_M|D_X|D_O, D_M|D_X|D_O|D_W, D_M|D_X|D_O, D_M|D_X|D_O|D_W,	/* D0 */
	D_IB, D_IB, 0, 0,						/* D4 */
	D_M, D_M, D_M, D_M,						/* D8 */
	D_M, D_M, D_M, D_M,						/* DC */
	D_J8, D_J8, D_J8, D_J8,					/* E0 */
	D_IB, D_IB|D_W, D_IB, D_IB|D_W,				/* E4 */
	D_JW, D_JW, D_FP, D_J8,					/* E8 */
	0, D_W, 0, D_W,						/* EC */
	0, 0, 0, 0,							/* F0 */
	0, D_O, D_M|D_O, D_M|D_W|D_O,					/* F4 */
	D_O, D_O, D_O, D_O,						/* F8 */
	D_O, D_O, D_M|D_O, D_M|D_W|D_O					/* FC */
};

/* The default segment for a mod r/m memory operand.  BP as a base means
 * the stack segment; everything else means the data segment.  rm = 6
 * with mod = 0 is not BP at all -- it is a direct address. */
static int defseg(mod, rm)
int mod, rm;
{
	if (rm == 2 || rm == 3)
		return (S_SS);		/* [BP+SI], [BP+DI]		*/
	if (rm == 6 && mod != 0)
		return (S_SS);		/* [BP+disp]			*/
	return (S_DS);
}

/*
 * Consume a mod r/m byte and whatever displacement it implies.  `n' is
 * the offset of the mod r/m byte within the instruction; returns the
 * offset just past the displacement.  Sets mod/reg/rm, IN_MEM, .disp
 * and .seg -- the last only when no explicit prefix already set it.
 */
static int modrm(cs, ip, n, in)
char *cs;
i16 ip;
int n;
struct i86in *in;
{
	register int b;

	b = fb(cs, ip, n);
	n++;
	in->mod = (i8)((b >> 6) & 3);
	in->reg = (i8)((b >> 3) & 7);
	in->rm = (i8)(b & 7);
	in->fl |= IN_MODRM;
	in->disp = 0;
	if (in->mod == 3)
		return (n);		/* a register, not memory	*/
	in->fl |= IN_MEM;
	if (in->mod == 0 && in->rm == 6) {
		in->disp = (i16)(fb(cs, ip, n) | (fb(cs, ip, n + 1) << 8));
		n += 2;
	} else if (in->mod == 1) {
		in->disp = sx(fb(cs, ip, n));
		n++;
	} else if (in->mod == 2) {
		in->disp = (i16)(fb(cs, ip, n) | (fb(cs, ip, n + 1) << 8));
		n += 2;
	}
	if (!(in->fl & IN_SEGOVR))
		in->seg = (i8)defseg(in->mod, in->rm);
	return (n);
}

/* Read a 16-bit immediate at offset n. */
static i16 iw(cs, ip, n)
char *cs;
i16 ip;
int n;
{
	return ((i16)(fb(cs, ip, n) | (fb(cs, ip, n + 1) << 8)));
}

/*
 * i86dec -- decode the instruction at cs:ip into *in.
 *
 * Returns the instruction length in bytes.  On a byte that is not an
 * 8086 opcode, sets in->op = I_BAD and returns the length consumed so
 * far (at least 1) so that a linear sweep can step past it -- a sweep
 * that stopped dead at the first table byte would report one finding
 * and hide the rest.
 */
int i86dec(cs, ip, in)
char *cs;
i16 ip;
struct i86in *in;
{
	register int op, n, f, k;

	in->fl = 0;
	in->mod = 0;
	in->reg = 0;
	in->rm = 0;
	in->seg = S_DS;
	in->disp = 0;
	in->imm = 0;
	in->imm2 = 0;
	n = 0;

	/* ---- prefixes.  A real 8086 accepts any number of them and only
	 * the last segment override counts; an interrupt between two
	 * prefixes loses all but the last, which is the famous bug and
	 * not something we need to reproduce.
	 *
	 * "Any number" is the one part that cannot be reproduced here.
	 * fb() WRAPS at sixteen bits, deliberately -- an instruction
	 * straddling 0xFFFF continues at 0 -- so a code segment filled
	 * with prefix bytes never leaves this loop, and the hang is
	 * inside one i86dec() call where the executor's step limit never
	 * gets a turn.  The bound is the architectural maximum
	 * instruction length, fifteen bytes including the opcode (Intel
	 * SDM Vol. 2, general instruction format: a longer encoding is
	 * #UD from the 386 on), so at most I86MAXPFX prefixes are
	 * consumed.  Nothing legal comes near it -- an 8086 instruction
	 * has a use for three, one segment override, one repeat and one
	 * LOCK -- so no legal instruction decodes differently for this.
	 * A sixteenth byte that is still a prefix falls out of the loop
	 * and reaches the grid as an opcode, where it is I_BAD. */
	op = cs[ip] & 0xff;
	while (ispfx[op]) {
		if (n >= I86MAXPFX) {
			op = fb(cs, ip, n);
			break;
		}
		op = fb(cs, ip, n);
		if (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e) {
			in->seg = (i8)((op >> 3) & 3);
			in->fl |= IN_SEGOVR;
			n++;
			continue;
		}
		if (op == 0xf0 || op == 0xf1) {	/* LOCK, and its alias	*/
			in->fl |= IN_LOCK;
			n++;
			continue;
		}
		if (op == 0xf2) {
			in->fl |= IN_REPNE;
			n++;
			continue;
		}
		if (op == 0xf3) {
			in->fl |= IN_REP;
			n++;
			continue;
		}
		break;
	}
	n++;					/* the opcode byte itself */

	/* ---- what the opcode byte alone settles.  D_I and D_D sit two
	 * bits above IN_IMM and IN_DIR so one shift places both. */
	f = (int)bform[op];
	in->op = bop[op];
	in->x = bx[op];
	in->fl |= (i8)((f >> 2) & (IN_IMM|IN_DIR));
	in->w = (i8)((f >> 10) & 1);
	if (f & D_M)
		n = modrm(cs, ip, n, in);
	if (f & D_3) {
		in->mod = 3;		/* .reg and .rm are already AX	*/
		if (f & D_L)
			in->rm = (i8)(op & 7);
	}
	if (f & D_X)
		in->x = in->reg;

	/* ---- and the operand it carries, if any. */
	k = f & D_IMM;
	if (k) {
		if (k == D_IB) {
			in->imm = (i16)fb(cs, ip, n);
			n++;
		} else if (k == D_IW) {
			in->imm = iw(cs, ip, n);
			n += 2;
		} else if (k == D_J8) {
			in->disp = sx(fb(cs, ip, n));
			n++;
			in->disp = (i16)(in->disp + ip + n);
		} else if (k == D_SB) {
			in->imm = sx(fb(cs, ip, n));
			n++;
		} else if (k == D_JW) {
			in->disp = iw(cs, ip, n);
			n += 2;
			in->disp = (i16)(in->disp + ip + n);
		} else if (k == D_DA) {
			/* A0-A3 carry no mod r/m, so the segment default is
			 * DS with no BP rule to consider; a prefix still
			 * overrides it. */
			in->fl |= IN_MODRM | IN_MEM;
			in->rm = 6;
			in->disp = iw(cs, ip, n);
			n += 2;
		} else {
			in->imm = iw(cs, ip, n);
			in->imm2 = iw(cs, ip, n + 2);
			n += 4;
		}
	}

	/* ---- the handful of opcodes a grid entry cannot finish.  Each
	 * one that needs a mod r/m byte has had it consumed already. */
	if (f & D_O) {
		if (op == 0xfe || op == 0xff) {
			switch (in->reg) {
			case 0: in->op = I_INC; break;
			case 1: in->op = I_DEC; break;
			case 2: if (op == 0xff) { in->op = I_CALLI; in->x = 0; }
				break;
			case 3: if (op == 0xff) { in->op = I_CALLI; in->x = 1; }
				break;
			case 4: if (op == 0xff) { in->op = I_JMPI; in->x = 0; }
				break;
			case 5: if (op == 0xff) { in->op = I_JMPI; in->x = 1; }
				break;
			case 6: if (op == 0xff) in->op = I_PUSH;
				break;
			default:
				break;			/* /7 -- and FE /2..7	*/
			}
		} else if (op >= 0xd0 && op <= 0xd3) {
			/* imm2 records where the count comes from: 0 = the
			 * literal 1, 1 = CL.  The 8086 does NOT mask that
			 * count -- masking to 5 bits arrived with the 186 --
			 * so a CL of 200 really is 200 iterations. */
			in->imm2 = (i16)(op & 2 ? 1 : 0);
		} else if (op == 0xf6 || op == 0xf7) {
			/* TEST-immediate, the two unaries, and the four
			 * widening multiply/divide forms, all under one
			 * mod r/m. */
			switch (in->reg) {
			case 0: case 1:			/* /1 aliases /0	*/
				in->op = I_TEST;
				in->fl |= IN_IMM;
				if (in->w) {
					in->imm = iw(cs, ip, n);
					n += 2;
				} else {
					in->imm = (i16)fb(cs, ip, n);
					n++;
				}
				break;
			case 2: in->op = I_NOT; break;
			case 3: in->op = I_NEG; break;
			default:
				in->op = I_MULDIV;
				in->x = in->reg;
				break;
			}
		} else if (op == 0x8c || op == 0x8e) {
			in->x = (i8)(in->reg & 3);
		} else if (op == 0xcc) {
			in->imm = 3;		/* the INT 3 breakpoint	*/
		} else if (op >= 0xfc) {
			in->imm = F_DF;
		} else if (op >= 0xfa) {
			in->imm = F_IF;
		} else {
			in->imm = F_CF;		/* F5, F8, F9		*/
		}
	}

	if (n < 1)
		n = 1;
	in->len = (i8)n;
	return (n);
}

/*
 * i86mnem -- the mnemonic for a decoded instruction, for the corpus
 * sweep and for a refusal message.  Not on any execution path; kept
 * here so the names sit next to the encodings that produce them.
 */
char *i86mnem(in)
struct i86in *in;
{
	static char *alu[8] = {
		"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"
	};
	static char *sh[8] = {
		"rol", "ror", "rcl", "rcr", "shl", "shr", "shl", "sar"
	};
	static char *md[8] = {
		"?", "?", "?", "?", "mul", "imul", "div", "idiv"
	};
	static char *cc[16] = {
		"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja",
		"js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg"
	};
	static char *lp[4] = { "loopne", "loope", "loop", "jcxz" };
	static char *st[5] = { "movs", "cmps", "stos", "lods", "scas" };

	switch (in->op) {
	case I_ALU:	return (alu[in->x & 7]);
	case I_SHIFT:	return (sh[in->x & 7]);
	case I_MULDIV:	return (md[in->x & 7]);
	case I_JCC:	return (cc[in->x & 15]);
	case I_LOOP:	return (lp[in->x & 3]);
	case I_STRING:	return (st[in->x < 5 ? in->x : 0]);
	case I_MOV: case I_MOVSR:	return ("mov");
	case I_LEA:	return ("lea");
	case I_LXS:	return (in->x == S_ES ? "les" : "lds");
	case I_XCHG:	return ("xchg");
	case I_TEST:	return ("test");
	case I_INC:	return ("inc");
	case I_DEC:	return ("dec");
	case I_NOT:	return ("not");
	case I_NEG:	return ("neg");
	case I_PUSH: case I_PUSHSR:	return ("push");
	case I_POP: case I_POPSR:	return ("pop");
	case I_PUSHF:	return ("pushf");
	case I_POPF:	return ("popf");
	case I_JMP: case I_JMPI: case I_JMPF:	return ("jmp");
	case I_CALL: case I_CALLI: case I_CALLF:	return ("call");
	case I_RET:	return ("ret");
	case I_RETF:	return ("retf");
	case I_INT:	return ("int");
	case I_INTO:	return ("into");
	case I_IRET:	return ("iret");
	case I_XLAT:	return ("xlat");
	case I_CBW:	return ("cbw");
	case I_CWD:	return ("cwd");
	case I_LAHF:	return ("lahf");
	case I_SAHF:	return ("sahf");
	case I_FLAG:
		if (in->imm == F_CF)
			return (in->x == 0 ? "clc" : in->x == 1 ? "stc" : "cmc");
		if (in->imm == F_IF)
			return (in->x ? "sti" : "cli");
		return (in->x ? "std" : "cld");
	case I_DAA:	return ("daa");
	case I_DAS:	return ("das");
	case I_AAA:	return ("aaa");
	case I_AAS:	return ("aas");
	case I_AAM:	return ("aam");
	case I_AAD:	return ("aad");
	case I_NOP:	return ("nop");
	case I_HLT:	return ("hlt");
	case I_WAIT:	return ("wait");
	case I_ESC:	return ("esc");
	case I_IO:	return ((in->x & 1) ? "out" : "in");
	}
	return ("(bad)");
}
