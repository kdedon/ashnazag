/*
 * i86test.c -- host tests for the CP/M-86 shim's 8086 decoder, executor,
 * .CMD loader and INT 0E0h seam (src/cmd/i86dec.c, i86exec.c, i86load.c,
 * i86bdos.c).
 *
 * Build: cc -DHOSTCC -o i86test i86test.c ../src/cmd/i86dec.c \
 *		../src/cmd/i86exec.c ../src/cmd/i86load.c ../src/cmd/i86bdos.c
 *
 * Coverage includes instruction lengths and execution, differential flags,
 * loader acceptance/refusal cases, real CMD files, generated malformed
 * fixtures, the INT E0h calling convention, and complete PIP/SUBMIT/GENCMD
 * runs against a stub CP/M. The `-c` mode inventories any additional CMD file.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>			/* the prefix section's deadline */
#include <unistd.h>

#include "i86.h"
#include "gdpb.h"
#include "conmode.h"
#include "gsx.h"

/* The native character control block functions 111 and 112 take. */
struct sccb {
	char	*a;
	i16	n;
};


/* The command tail sections 6 and 7 load with; tools/mkcmdfix.py writes
 * the same string, and RUN8080.CMD reads its length back out of the base
 * page it lands in. */
#define FIXTAIL "B:FIX.DAT"

static int nfail, ntest;

static void fail(const char *what, long got, long want)
{
	printf("FAIL %-40s got %ld (0x%lx) want %ld (0x%lx)\n",
		what, got, (unsigned long)got, want, (unsigned long)want);
	nfail++;
}

static void chk(const char *what, long got, long want)
{
	ntest++;
	if (got != want)
		fail(what, got, want);
}

/* ================================================================== */
/* 1. lengths							      */
/* ================================================================== */

/*
 * Transcribed from the 8086 encoding by hand, not generated from the
 * decoder -- a table the decoder produced would agree with it by
 * construction and prove nothing.  Each row is the byte string, its
 * length, and the mnemonic i86mnem() must name.
 *
 * The rows chosen are the ones where a length can go wrong: every mod
 * field, the mod=00/rm=110 direct form that is the only two-byte
 * displacement in an otherwise displacement-free mod, the sign-extended
 * 0x83 immediate against the full 0x81 one, the F6/F7 group where /0
 * carries an immediate and /2../7 do not, the FF group, prefixes, and
 * the far forms.
 */
struct lrow {
	unsigned char b[8];
	int n;			/* bytes present in b[]			*/
	int len;		/* the length i86dec must report	*/
	const char *m;
};

static struct lrow lrows[] = {
	/* --- mod r/m, all four mods, on one opcode --- */
	{{0x8b, 0x00}, 2, 2, "mov"},			/* mov ax,[bx+si]  */
	{{0x8b, 0x06, 0x34, 0x12}, 4, 4, "mov"},	/* mov ax,[0x1234] */
	{{0x8b, 0x40, 0x08}, 3, 3, "mov"},		/* mov ax,[bx+si+8]*/
	{{0x8b, 0x80, 0x34, 0x12}, 4, 4, "mov"},	/* mov ax,[bx+si+d16] */
	{{0x8b, 0xc3}, 2, 2, "mov"},			/* mov ax,bx	   */
	/* --- the direct form is mod=00 rm=110 and nothing else --- */
	{{0x8b, 0x46, 0x04}, 3, 3, "mov"},		/* mov ax,[bp+4]   */
	{{0x8b, 0x86, 0x34, 0x12}, 4, 4, "mov"},	/* mov ax,[bp+d16] */
	/* --- immediates --- */
	{{0x04, 0x7f}, 2, 2, "add"},			/* add al,imm8	   */
	{{0x05, 0x34, 0x12}, 3, 3, "add"},		/* add ax,imm16	   */
	{{0x80, 0x3e, 0x00, 0x01, 0x20}, 5, 5, "cmp"},	/* cmp [d16],imm8  */
	{{0x81, 0x3e, 0x00, 0x01, 0x34, 0x12}, 6, 6, "cmp"},
	{{0x83, 0xc4, 0x06}, 3, 3, "add"},		/* add sp,+6	   */
	{{0xc6, 0x06, 0x00, 0x01, 0x41}, 5, 5, "mov"},	/* mov [d16],imm8  */
	{{0xc7, 0x06, 0x00, 0x01, 0x34, 0x12}, 6, 6, "mov"},
	{{0xb0, 0x1a}, 2, 2, "mov"},
	{{0xb8, 0x34, 0x12}, 3, 3, "mov"},
	/* --- F6/F7: /0 carries an immediate, /2../7 do not --- */
	{{0xf6, 0xc3, 0x01}, 3, 3, "test"},		/* test bl,1	   */
	{{0xf7, 0xc3, 0x34, 0x12}, 4, 4, "test"},	/* test bx,0x1234  */
	{{0xf6, 0xd3}, 2, 2, "not"},
	{{0xf7, 0xdb}, 2, 2, "neg"},
	{{0xf7, 0xe3}, 2, 2, "mul"},
	{{0xf7, 0xfb}, 2, 2, "idiv"},
	{{0xf6, 0x16, 0x00, 0x01}, 4, 4, "not"},	/* not byte [d16]  */
	/* --- FF and FE groups --- */
	{{0xff, 0x36, 0x00, 0x01}, 4, 4, "push"},
	{{0xff, 0x06, 0x00, 0x01}, 4, 4, "inc"},
	{{0xff, 0xd3}, 2, 2, "call"},
	{{0xff, 0x1e, 0x00, 0x01}, 4, 4, "call"},	/* far indirect	   */
	{{0xfe, 0xc0}, 2, 2, "inc"},
	/* --- branches --- */
	{{0x74, 0x10}, 2, 2, "je"},
	{{0xeb, 0xfe}, 2, 2, "jmp"},
	{{0xe9, 0x34, 0x12}, 3, 3, "jmp"},
	{{0xe8, 0x34, 0x12}, 3, 3, "call"},
	{{0xea, 0x00, 0x01, 0x00, 0x20}, 5, 5, "jmp"},	/* far direct	   */
	{{0x9a, 0x00, 0x01, 0x00, 0x20}, 5, 5, "call"},
	{{0xc2, 0x04, 0x00}, 3, 3, "ret"},
	{{0xc3}, 1, 1, "ret"},
	{{0xcb}, 1, 1, "retf"},
	{{0xca, 0x04, 0x00}, 3, 3, "retf"},
	/* --- segment things --- */
	{{0x8c, 0xd9}, 2, 2, "mov"},			/* mov cx,ds	   */
	{{0x8e, 0xd1}, 2, 2, "mov"},			/* mov ss,cx	   */
	{{0xc4, 0x5e, 0x06}, 3, 3, "les"},
	{{0xc5, 0x5e, 0x06}, 3, 3, "lds"},
	{{0x06}, 1, 1, "push"},
	{{0x1f}, 1, 1, "pop"},
	/* --- prefixes add exactly their own length --- */
	{{0x2e, 0x8c, 0x16, 0x5b, 0x00}, 5, 5, "mov"},	/* mov cs:[d16],ss */
	{{0xf3, 0xa4}, 2, 2, "movs"},
	{{0xf2, 0xae}, 2, 2, "scas"},
	{{0xf0, 0x26, 0x8b, 0x07}, 4, 4, "mov"},	/* two prefixes	   */
	/* --- shifts: the count is in the opcode, never in a byte --- */
	{{0xd1, 0xe0}, 2, 2, "shl"},
	{{0xd3, 0xd8}, 2, 2, "rcr"},
	{{0xd0, 0x2e, 0x00, 0x01}, 4, 4, "shr"},
	/* --- one-byte oddments --- */
	{{0x90}, 1, 1, "nop"},
	{{0x98}, 1, 1, "cbw"},
	{{0x99}, 1, 1, "cwd"},
	{{0x9c}, 1, 1, "pushf"},
	{{0x27}, 1, 1, "daa"},
	{{0xd7}, 1, 1, "xlat"},
	{{0xcd, 0xe0}, 2, 2, "int"},
	{{0xcc}, 1, 1, "int"},
	{{0xd4, 0x0a}, 2, 2, "aam"},
	{{0xd5, 0x0a}, 2, 2, "aad"},
	{{0xf4}, 1, 1, "hlt"},
	{{0xfc}, 1, 1, "cld"},
	{{0x8d, 0x26, 0x84, 0x01}, 4, 4, "lea"},	/* PIP's prologue  */
	/* --- the 8086's holes, honoured rather than idealised --- */
	{{0x0f}, 1, 1, "pop"},				/* 0F is POP CS	   */
	{{0x62, 0x10}, 2, 2, "jb"},			/* 62 aliases 72   */
	{{0x82, 0xc3, 0x01}, 3, 3, "add"},		/* 82 aliases 80   */
	{{0xd8, 0x06, 0x00, 0x01}, 4, 4, "esc"},
};
#define NLROWS (sizeof lrows / sizeof lrows[0])

static void t_lengths(void)
{
	static char seg[65536];
	struct i86in in;
	unsigned i;
	int n;
	char nm[64];

	for (i = 0; i < NLROWS; i++) {
		memset(seg, 0, 16);
		memcpy(seg, lrows[i].b, lrows[i].n);
		n = i86dec(seg, (i16)0, &in);
		sprintf(nm, "len[%u] %02x %02x", i, lrows[i].b[0],
			lrows[i].b[1]);
		chk(nm, n, lrows[i].len);
		chk(nm, in.len, lrows[i].len);
		ntest++;
		if (strcmp(i86mnem(&in), lrows[i].m) != 0) {
			printf("FAIL %-40s got \"%s\" want \"%s\"\n",
				nm, i86mnem(&in), lrows[i].m);
			nfail++;
		}
	}
}

/*
 * Every one of the 256 opcode bytes must decode to SOMETHING with a
 * length of at least one, and the length must not run past the eight
 * bytes any 8086 instruction can occupy.  This is the property that
 * makes a linear sweep terminate, and it is worth asserting separately
 * from the table above because the table cannot cover 256 rows.
 */
static void t_allbytes(void)
{
	static char seg[65536];
	struct i86in in;
	int op, n, nbad;

	nbad = 0;
	for (op = 0; op < 256; op++) {
		memset(seg, 0, 16);
		seg[0] = (char)op;
		n = i86dec(seg, (i16)0, &in);
		ntest++;
		if (n < 1 || n > 8) {
			printf("FAIL opcode %02x length %d\n", op, n);
			nfail++;
		}
		if (in.op == I_BAD)
			nbad++;
	}
	/* 0xD6 (SALC) and 0xF1 are the only bytes with no instruction
	 * behind them; 0xF1 reaches the default arm because it is
	 * consumed as a LOCK prefix and then the following zero byte
	 * decodes as ADD.  So exactly one byte is I_BAD standing alone. */
	chk("bytes with no instruction", nbad, 1);
}

/* ================================================================== */
/* 2. flags, differentially					      */
/* ================================================================== */

/*
 * The reference.  Written from the definitions, in 32-bit arithmetic,
 * with no shared expression with i86exec.c's lazy formulae -- the whole
 * value of a differential is that the two sides were arrived at
 * separately.  AF here is "was there a carry out of bit 3", computed by
 * doing the low nibble's arithmetic on its own; i86exec.c gets it from
 * a^b^r, which is the same fact reached the other way round.
 */
#define A_ADD 0
#define A_OR  1
#define A_ADC 2
#define A_SBB 3
#define A_AND 4
#define A_SUB 5
#define A_XOR 6
#define A_CMP 7

static int refpar(unsigned v)
{
	int n = 0;
	v &= 0xff;
	while (v) { n += v & 1; v >>= 1; }
	return ((n & 1) == 0);
}

static unsigned refflags(int aop, int w, unsigned a, unsigned b, int cin,
			 unsigned *rp)
{
	unsigned msk = w ? 0xffffu : 0xffu;
	unsigned msb = w ? 0x8000u : 0x80u;
	long sa, sb, sr;
	unsigned r;
	unsigned f = 0;

	a &= msk;
	b &= msk;
	sa = (a & msb) ? (long)a - (long)(msk + 1) : (long)a;
	sb = (b & msb) ? (long)b - (long)(msk + 1) : (long)b;

	switch (aop) {
	case A_ADD: cin = 0;			/* fall through	*/
	case A_ADC:
		r = (a + b + cin) & msk;
		if ((unsigned long)a + b + cin > msk)
			f |= F_CF;
		if ((a & 15) + (b & 15) + cin > 15)
			f |= F_AF;
		sr = sa + sb + cin;
		if (sr > (long)(msb - 1) || sr < -(long)msb)
			f |= F_OF;
		break;
	case A_SUB: case A_CMP: cin = 0;	/* fall through	*/
	case A_SBB:
		r = (a - b - cin) & msk;
		if ((unsigned long)b + cin > a)
			f |= F_CF;
		if ((unsigned)(b & 15) + cin > (a & 15))
			f |= F_AF;
		sr = sa - sb - cin;
		if (sr > (long)(msb - 1) || sr < -(long)msb)
			f |= F_OF;
		break;
	case A_AND: r = (a & b) & msk; break;
	case A_OR:  r = (a | b) & msk; break;
	default:    r = (a ^ b) & msk; break;
	}
	if (r == 0)
		f |= F_ZF;
	if (r & msb)
		f |= F_SF;
	if (refpar(r))
		f |= F_PF;
	*rp = r;
	return (f);
}

/*
 * Drive one ALU instruction through i86step and read the flags back.
 * `aop' is the 8086's own sub-op number, so the encoding is 0x80|w with
 * reg = aop, which is the form the executor reaches by the widest path
 * (mod r/m plus an immediate plus a lazy record plus a materialisation).
 */
static char fseg[65536];
static struct i86 fm;

static unsigned run_alu(int aop, int w, unsigned a, unsigned b, int cin,
			unsigned *rp)
{
	struct i86in in;
	int i;

	memset(&fm, 0, sizeof fm);
	for (i = 0; i < 4; i++)
		fm.sb[i] = fseg;
	fm.lz = LZ_NONE;
	fm.fl = (i16)(F_ONES | (cin ? F_CF : 0));
	fm.ip = 0;
	fm.r[R_AX] = (i16)a;
	/* 80 /aop c0  ib   -- AL, imm8      (mod=3, rm=0)
	 * 81 /aop c0  iw   -- AX, imm16 */
	fseg[0] = (char)(w ? 0x81 : 0x80);
	fseg[1] = (char)(0xc0 | (aop << 3));
	fseg[2] = (char)(b & 0xff);
	if (w)
		fseg[3] = (char)((b >> 8) & 0xff);
	i86step(&fm, &in);
	*rp = (unsigned)(w ? (fm.r[R_AX] & 0xffff) : (fm.r[R_AX] & 0xff));
	return ((unsigned)(i86flags(&fm) & F_LAZY));
}

static void t_flags_byte(void)
{
	static int ops[6] = { A_ADD, A_ADC, A_SUB, A_SBB, A_AND, A_XOR };
	unsigned a, b, gf, wf, gr, wr;
	int o, cin, bad;
	char nm[64];

	for (o = 0; o < 6; o++) {
		bad = 0;
		for (cin = 0; cin < 2; cin++) {
			for (a = 0; a < 256; a++) {
				for (b = 0; b < 256; b++) {
					gf = run_alu(ops[o], 0, a, b, cin, &gr);
					wf = refflags(ops[o], 0, a, b, cin, &wr);
					if (gf != wf || gr != wr) {
						if (bad++ < 4)
						  printf("FAIL alu%d b a=%02x "
							"b=%02x c=%d: r %02x/%02x "
							"f %03x/%03x\n",
							ops[o], a, b, cin,
							gr, wr, gf, wf);
					}
				}
			}
		}
		ntest++;
		sprintf(nm, "flags byte aop=%d (131072 cases)", ops[o]);
		if (bad) {
			printf("FAIL %s: %d mismatches\n", nm, bad);
			nfail++;
		}
	}
}

/*
 * Words, sampled rather than exhausted: 4 G cases would take hours and
 * buy nothing the byte sweep did not, because the only thing that
 * changes with width is the mask and the sign bit.  The spread is
 * chosen for the boundaries -- zero, one, the sign bit either side, the
 * top, and a couple of values with interesting nibbles.
 */
static void t_flags_word(void)
{
	static unsigned v[] = {
		0x0000, 0x0001, 0x000f, 0x0010, 0x007f, 0x0080, 0x00ff,
		0x0100, 0x7ffe, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff,
		0x1234, 0xabcd, 0x5a5a, 0xa5a5
	};
	static int ops[8] = { A_ADD, A_OR, A_ADC, A_SBB,
			      A_AND, A_SUB, A_XOR, A_CMP };
	unsigned gf, wf, gr, wr;
	int o, cin, i, j, bad;

	bad = 0;
	for (o = 0; o < 8; o++)
		for (cin = 0; cin < 2; cin++)
			for (i = 0; i < 18; i++)
				for (j = 0; j < 18; j++) {
					gf = run_alu(ops[o], 1, v[i], v[j],
						     cin, &gr);
					wf = refflags(ops[o], 1, v[i], v[j],
						      cin, &wr);
					if (ops[o] == A_CMP)
						gr = wr;   /* stores nothing */
					if (gf != wf || gr != wr) {
						if (bad++ < 6)
						  printf("FAIL aluw%d a=%04x "
							"b=%04x c=%d: r "
							"%04x/%04x f %03x/%03x\n",
							ops[o], v[i], v[j], cin,
							gr, wr, gf, wf);
					}
				}
	ntest++;
	if (bad) {
		printf("FAIL flags word: %d mismatches\n", bad);
		nfail++;
	}
}

/*
 * INC and DEC are the one place the 8086's flag rules are not uniform:
 * they set the other five and leave CF exactly as they found it.  A
 * lazy scheme is precisely where that gets lost -- the record has to be
 * folded before the new one replaces it -- so it gets its own sweep
 * over every byte value and both carry states.
 */
static void t_incdec(void)
{
	struct i86in in;
	unsigned a, gf, wf, wr;
	int cin, isdec, bad, i;

	bad = 0;
	for (isdec = 0; isdec < 2; isdec++)
		for (cin = 0; cin < 2; cin++)
			for (a = 0; a < 256; a++) {
				memset(&fm, 0, sizeof fm);
				for (i = 0; i < 4; i++)
					fm.sb[i] = fseg;
				fm.lz = LZ_NONE;
				fm.fl = (i16)(F_ONES | (cin ? F_CF : 0));
				fm.r[R_AX] = (i16)a;
				fseg[0] = (char)0xfe;		/* FE /0, /1 */
				fseg[1] = (char)(isdec ? 0xc8 : 0xc0);
				i86step(&fm, &in);
				gf = (unsigned)(i86flags(&fm) & F_LAZY);
				wf = refflags(isdec ? A_SUB : A_ADD, 0,
					      a, 1, 0, &wr);
				wf &= ~F_CF;
				if (cin)
					wf |= F_CF;
				if (gf != wf
				 || (unsigned)(fm.r[R_AX] & 0xff) != wr) {
					if (bad++ < 4)
						printf("FAIL %s a=%02x c=%d: "
							"r %02x/%02x f %03x/%03x\n",
							isdec ? "dec" : "inc",
							a, cin,
							fm.r[R_AX] & 0xff, wr,
							gf, wf);
				}
			}
	ntest++;
	if (bad) {
		printf("FAIL inc/dec: %d mismatches\n", bad);
		nfail++;
	}
}

/*
 * i86lcond() against i86cond(i86flags()) for every class and all
 * sixteen conditions: bytes exhaustively (with junk above the byte),
 * words over the boundary values above plus 100,000 random pairs.
 * The record must come back untouched, so a later PUSHF still folds it.
 */
static long lcbad[6];
static unsigned long lcseed = 1;

static unsigned lcrand(void)
{
	lcseed = lcseed * 1103515245UL + 12345UL;
	return ((unsigned)(lcseed >> 16) & 0xffff);
}

static void lcone(int cls, int w, unsigned a, unsigned b, unsigned r,
		  int c, unsigned fl)
{
	struct i86 m, m2;
	int cc, want;

	memset(&m, 0, sizeof m);
	m.lz = (i8)cls;
	m.lw = (i8)w;
	m.la = (i16)a;
	m.lb = (i16)b;
	m.lr = (i16)r;
	m.lc = (i8)c;
	m.fl = (i16)(fl | F_ONES);
	for (cc = 0; cc < 16; cc++) {
		m2 = m;
		want = i86cond((int)i86flags(&m2), cc);
		m2 = m;
		if (i86lcond(&m2, cc) != want
		 || memcmp(&m2, &m, sizeof m) != 0) {
			if (lcbad[cls]++ == 0)
				printf("i86test: lcond class %d w %d cc %d "
					"a %04x b %04x r %04x c %d fl %04x\n",
					cls, w, cc, m.la, m.lb, m.lr, c, fl);
		}
	}
}

/* Every class on (a, b, carry-in); fl is the state INC/DEC preserve. */
static void lcops(int w, unsigned a, unsigned b, int c, unsigned fl)
{
	unsigned k;

	k = w ? 0 : lcrand() & 0xff00;	/* a byte record's junk high half */
	lcone(LZ_ADD, w, a | k, b | k, (a + b + c) | k, c, 0);
	lcone(LZ_SUB, w, a | k, b | k, (a - b - c) & 0xffff, c, 0);
	if (c)
		return;
	lcone(LZ_LOG, w, a, b, a & b, 0, 0);
	lcone(LZ_LOG, w, a, b, a ^ b, 0, 0);
	lcone(LZ_INC, w, a, 1, a + 1, 0, fl);
	lcone(LZ_DEC, w, a, 1, a - 1, 0, fl);
}

static void t_lazycond(void)
{
	static unsigned v[] = {
		0x0000, 0x0001, 0x000f, 0x0010, 0x007f, 0x0080, 0x00ff,
		0x0100, 0x7ffe, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff,
		0x1234, 0xabcd, 0x5a5a, 0xa5a5
	};
	unsigned a, b;
	int c, i, j;
	i32 n0;

	n0 = i86nflag;
	for (a = 0; a < 0x1000; a++)
		lcone(LZ_NONE, 0, 0, 0, 0, 0, a & F_LAZY);
	for (a = 0; a < 256; a++)
		for (b = 0; b < 256; b++)
			for (c = 0; c < 2; c++)
				lcops(0, a, b, c, (b << 4 | b) & F_LAZY);
	for (i = 0; i < 18; i++)
		for (j = 0; j < 18; j++)
			for (c = 0; c < 2; c++)
				lcops(1, v[i], v[j], c, v[j] & F_LAZY);
	for (i = 0; i < 100000; i++) {
		a = lcrand();
		b = lcrand();
		lcops(1, a, b, i & 1, lcrand() & F_LAZY);
	}
	i86nflag = n0;
	for (c = 0; c <= LZ_DEC; c++)
		chk("lazy condition matches the materialised one",
			lcbad[c], 0L);
}

/* NEG is SUB from zero, and its CF rule ("set unless the operand was
 * zero") is the one people get wrong, so it is swept too. */
static void t_neg(void)
{
	struct i86in in;
	unsigned a, gf, wf, wr;
	int bad, i;

	bad = 0;
	for (a = 0; a < 256; a++) {
		memset(&fm, 0, sizeof fm);
		for (i = 0; i < 4; i++)
			fm.sb[i] = fseg;
		fm.lz = LZ_NONE;
		fm.fl = F_ONES;
		fm.r[R_AX] = (i16)a;
		fseg[0] = (char)0xf6;
		fseg[1] = (char)0xd8;			/* neg al	*/
		i86step(&fm, &in);
		gf = (unsigned)(i86flags(&fm) & F_LAZY);
		wf = refflags(A_SUB, 0, 0, a, 0, &wr);
		if (gf != wf || (unsigned)(fm.r[R_AX] & 0xff) != wr) {
			if (bad++ < 4)
				printf("FAIL neg a=%02x: r %02x/%02x "
					"f %03x/%03x\n", a,
					fm.r[R_AX] & 0xff, wr, gf, wf);
		}
	}
	ntest++;
	if (bad) {
		printf("FAIL neg: %d mismatches\n", bad);
		nfail++;
	}
}

/*
 * Delay loops.  A register DEC and a JNZ back, run with the fast path and
 * without it, must end in the same machine, memory and counts.  A leading
 * ADD AL leaves a pending record for the first DEC to fold.
 */
static int dwaits, dlast;

static int dwait(int n)
{
	dwaits++;
	dlast = n;
	return (0);
}

/* Run to the HLT; answer the steps taken. */
static long drun(void)
{
	struct i86in in;
	long k;

	for (k = 0; k < 200000L; k++)
		if (i86step(&fm, &in) != X_OK)
			break;
	return (k);
}

/* form 0: FE /1 byte, 1: FF /1 word, 2: 48+r word */
static void dload(int form, int x, int pre, unsigned v, int cin)
{
	int i, p;

	memset(&fm, 0, sizeof fm);
	for (i = 0; i < 4; i++)
		fm.sb[i] = fseg;
	for (i = 0; i < 0x10000; i += 7)
		fseg[i] = (char)(i * 13);
	fm.lz = LZ_NONE;
	fm.fl = (i16)(F_ONES | (cin ? F_CF : 0));
	for (i = 0; i < 8; i++)
		fm.r[i] = (i16)(0x1111 * (i + 1));
	p = 0x100;
	if (pre) {
		fseg[p++] = (char)0x04;		/* add al,80h		*/
		fseg[p++] = (char)0x80;
	}
	if (form == 2)
		fseg[p++] = (char)(0x48 + x);
	else {
		fseg[p++] = (char)(0xfe + form);
		fseg[p++] = (char)(0xc8 + x);
	}
	fseg[p] = (char)0x75;			/* jnz back		*/
	fseg[p + 1] = (char)(form == 2 ? 0xfd : 0xfc);
	fseg[p + 2] = (char)0xf4;		/* hlt			*/
	fm.ip = 0x100;
	if (form == 0 && (x & 4))
		fm.r[x & 3] = (i16)((fm.r[x & 3] & 0x00ff) | (v << 8));
	else if (form == 0)
		fm.r[x] = (i16)((fm.r[x] & 0xff00) | v);
	else
		fm.r[x] = (i16)v;
}

static void t_delay(void)
{
	static const unsigned v8[] = { 0, 1, 2, 3, 127, 128, 254, 255 };
	static const unsigned v16[] = { 0, 1, 2, 255, 256, 0x8000, 0xffff };
	static char ref[65536];
	struct i86 want;
	i32 wi, wf;
	long ws, gs;
	int form, x, pre, cin, vi, nv, bad, ncase;
	unsigned v;

	bad = ncase = 0;
	for (form = 0; form < 3; form++)
	for (x = 0; x < 8; x++)
	for (pre = 0; pre < 2; pre++)
	for (cin = 0; cin < 2; cin++) {
		nv = form == 0 ? 8 : 7;
		for (vi = 0; vi < nv; vi++) {
			v = form == 0 ? v8[vi] : v16[vi];
			ncase++;
			dload(form, x, pre, v, cin);
			i86fast = 0;
			i86ninsn = i86nflag = i86nskip = 0;
			ws = drun();
			wi = i86ninsn;
			wf = i86nflag;
			want = fm;
			memcpy(ref, fseg, sizeof ref);

			dload(form, x, pre, v, cin);
			i86fast = 1;
			i86ninsn = i86nflag = i86nskip = 0;
			gs = drun();
			if (memcmp(&fm, &want, sizeof fm) != 0
			 || memcmp(fseg, ref, sizeof ref) != 0
			 || i86ninsn != wi || i86nflag != wf
			 || gs + (long)i86nskip != ws) {
				if (bad++ < 4)
					printf("FAIL delay form%d r%d pre%d "
						"cf%d v%u: insn %lu/%lu flag "
						"%lu/%lu steps %ld+%lu/%ld "
						"ip %04x/%04x fl %03x/%03x\n",
						form, x, pre, cin, v,
						(unsigned long)i86ninsn,
						(unsigned long)wi,
						(unsigned long)i86nflag,
						(unsigned long)wf, gs,
						(unsigned long)i86nskip, ws,
						fm.ip, want.ip, fm.fl, want.fl);
			}
		}
	}
	ntest++;
	if (bad) {
		printf("FAIL delay loops: %d of %d cases differ\n", bad, ncase);
		nfail++;
	}
	chk("delay loops compared", (long)ncase, 8L * 2 * 2 * (8 + 7 + 7));

	/* The time.  256 turns of DEC AL are 4852 clocks and 65536 of
	 * DEC AX 1179636; a tick is 47700.  Each sleep asks one tick more
	 * than it owes. */
	i86wait = dwait;
	for (vi = 0; vi < 10; vi++) {
		dload(0, 0, 0, 0, 0);
		drun();
		if (vi == 8)
			chk("delay: no sleep below a tick", (long)dwaits, 0L);
	}
	chk("delay: 48520 clocks sleep once", (long)dwaits, 1L);
	chk("delay: ... asking two ticks", (long)dlast, 2L);
	dload(2, 0, 0, 0, 0);
	drun();
	chk("delay: 820 + 1179636 sleeps again", (long)dwaits, 2L);
	chk("delay: ... asking 25 ticks", (long)dlast, 25L);
	i86wait = 0;
	i86nskip = 0;
}

/* ================================================================== */
/* 3. execution							      */
/* ================================================================== */

static char xseg[65536];
static char xseg2[65536];
static struct i86 xm;

static void xsetup(void)
{
	int i;

	memset(xseg, 0, sizeof xseg);
	memset(&xm, 0, sizeof xm);
	for (i = 0; i < 4; i++) {
		xm.sb[i] = xseg;
		xm.sr[i] = 0x1000;
	}
	xm.lz = LZ_NONE;
	xm.fl = F_ONES;
	xm.ip = 0x100;
	xm.r[R_SP] = 0xff00;
	i86nseg = 1;
	i86spar[0] = 0x1000;
	i86sbase[0] = xseg;
}

static int xstep(void)
{
	struct i86in in;

	return (i86step(&xm, &in));
}

static void t_exec(void)
{

	/* --- mov r16,imm / mov [mem],r16 / mov r16,[mem] --- */
	xsetup();
	xseg[0x100] = (char)0xb8; xseg[0x101] = 0x34; xseg[0x102] = 0x12;
	chk("mov ax,imm rc", xstep(), X_OK);
	chk("mov ax,imm", xm.r[R_AX] & 0xffff, 0x1234);
	chk("mov ax,imm ip", xm.ip & 0xffff, 0x103);

	xsetup();
	xm.r[R_AX] = 0xbeef;
	xseg[0x100] = (char)0xa3; xseg[0x101] = 0x00; xseg[0x102] = 0x20;
	chk("mov [d16],ax rc", xstep(), X_OK);
	chk("mov [d16],ax lo", xseg[0x2000] & 0xff, 0xef);
	chk("mov [d16],ax hi", xseg[0x2001] & 0xff, 0xbe);

	xsetup();
	xseg[0x2000] = 0x21; xseg[0x2001] = 0x43;
	xseg[0x100] = (char)0xa1; xseg[0x101] = 0x00; xseg[0x102] = 0x20;
	chk("mov ax,[d16]", (xstep(), xm.r[R_AX] & 0xffff), 0x4321);

	/* --- the mod=01 [bp+d8] form, which defaults to SS --- */
	xsetup();
	xm.r[R_BP] = 0x3000;
	xseg[0x3004] = 0x77; xseg[0x3005] = 0x66;
	xseg[0x100] = (char)0x8b; xseg[0x101] = 0x56; xseg[0x102] = 0x04;
	chk("mov dx,[bp+4]", (xstep(), xm.r[R_DX] & 0xffff), 0x6677);

	/* --- push/pop, and the stack really moves --- */
	xsetup();
	xm.r[R_BX] = 0xcafe;
	xseg[0x100] = 0x53;			/* push bx		*/
	chk("push bx rc", xstep(), X_OK);
	chk("push bx sp", xm.r[R_SP] & 0xffff, 0xfefe);
	chk("push bx mem", ((xseg[0xfefe] & 0xff)
			  | ((xseg[0xfeff] & 0xff) << 8)), 0xcafe);
	xseg[0x101] = 0x59;			/* pop cx		*/
	chk("pop cx rc", xstep(), X_OK);
	chk("pop cx", xm.r[R_CX] & 0xffff, 0xcafe);
	chk("pop cx sp", xm.r[R_SP] & 0xffff, 0xff00);

	/* --- call/ret, the pair the corpus leans on hardest --- */
	xsetup();
	xseg[0x100] = (char)0xe8; xseg[0x101] = 0x0d; xseg[0x102] = 0x00;
	chk("call rel rc", xstep(), X_OK);
	chk("call rel ip", xm.ip & 0xffff, 0x110);
	chk("call rel pushed", ((xseg[0xfefe] & 0xff)
			      | ((xseg[0xfeff] & 0xff) << 8)), 0x103);
	xseg[0x110] = (char)0xc2; xseg[0x111] = 0x04; xseg[0x112] = 0x00;
	chk("ret 4 rc", xstep(), X_OK);
	chk("ret 4 ip", xm.ip & 0xffff, 0x103);
	chk("ret 4 sp", xm.r[R_SP] & 0xffff, 0xff04);

	/* --- a backward short jump, the loop shape --- */
	xsetup();
	xm.ip = 0x110;
	xseg[0x110] = (char)0xeb; xseg[0x111] = (char)0xee;	/* -18	*/
	chk("jmp short back", (xstep(), xm.ip & 0xffff), 0x100);

	/* --- Jcc taken and not taken off a real compare --- */
	xsetup();
	xm.r[R_AX] = 5;
	xseg[0x100] = 0x3c; xseg[0x101] = 0x05;		/* cmp al,5	*/
	xseg[0x102] = 0x74; xseg[0x103] = 0x10;		/* je +0x10	*/
	xstep();
	chk("je taken", (xstep(), xm.ip & 0xffff), 0x114);
	xsetup();
	xm.r[R_AX] = 4;
	xseg[0x100] = 0x3c; xseg[0x101] = 0x05;
	xseg[0x102] = 0x74; xseg[0x103] = 0x10;
	xstep();
	chk("je not taken", (xstep(), xm.ip & 0xffff), 0x104);

	/* --- LOOP decrements CX and JCXZ does not --- */
	xsetup();
	xm.r[R_CX] = 3;
	xseg[0x100] = (char)0xe2; xseg[0x101] = (char)0xfe;
	chk("loop rc", xstep(), X_OK);
	chk("loop cx", xm.r[R_CX] & 0xffff, 2);
	chk("loop ip", xm.ip & 0xffff, 0x100);
	xsetup();
	xm.r[R_CX] = 1;
	xseg[0x100] = (char)0xe2; xseg[0x101] = (char)0xfe;
	xstep();
	chk("loop falls through", xm.ip & 0xffff, 0x102);
	xsetup();
	xm.r[R_CX] = 5;
	xseg[0x100] = (char)0xe3; xseg[0x101] = 0x10;
	xstep();
	chk("jcxz leaves cx", xm.r[R_CX] & 0xffff, 5);
	chk("jcxz not taken", xm.ip & 0xffff, 0x102);

	/* --- LEA computes the address and touches no memory --- */
	xsetup();
	xm.r[R_BX] = 0x0200; xm.r[R_SI] = 0x0030;
	xseg[0x100] = (char)0x8d; xseg[0x101] = 0x40; xseg[0x102] = 0x04;
	chk("lea ax,[bx+si+4]", (xstep(), xm.r[R_AX] & 0xffff), 0x234);

	/* --- XCHG both ways --- */
	xsetup();
	xm.r[R_AX] = 0x1111; xm.r[R_BX] = 0x2222;
	xseg[0x100] = (char)0x93;			/* xchg ax,bx	*/
	xstep();
	chk("xchg ax", xm.r[R_AX] & 0xffff, 0x2222);
	chk("xchg bx", xm.r[R_BX] & 0xffff, 0x1111);

	/* --- the byte register file: AH is the top of AX --- */
	xsetup();
	xm.r[R_AX] = 0x1234;
	xseg[0x100] = (char)0xb4; xseg[0x101] = (char)0xab;	/* mov ah,ab */
	xstep();
	chk("mov ah,imm", xm.r[R_AX] & 0xffff, 0xab34);
	xseg[0x102] = (char)0xb0; xseg[0x103] = (char)0xcd;	/* mov al,cd */
	xstep();
	chk("mov al,imm", xm.r[R_AX] & 0xffff, 0xabcd);

	/* --- shifts: count 1, count from CL, and count 0 --- */
	xsetup();
	xm.r[R_AX] = 0x8001;
	xseg[0x100] = (char)0xd1; xseg[0x101] = (char)0xe0;	/* shl ax,1 */
	xstep();
	chk("shl ax,1", xm.r[R_AX] & 0xffff, 0x0002);
	chk("shl ax,1 cf", (i86flags(&xm) & F_CF) != 0, 1);
	xsetup();
	xm.r[R_AX] = 0x1000; xm.r[R_CX] = 4;
	xseg[0x100] = (char)0xd3; xseg[0x101] = (char)0xe8;	/* shr ax,cl */
	xstep();
	chk("shr ax,cl", xm.r[R_AX] & 0xffff, 0x0100);
	xsetup();
	xm.r[R_AX] = 0x1234; xm.r[R_CX] = 0;
	xm.fl = (i16)(F_ONES | F_CF);
	xseg[0x100] = (char)0xd3; xseg[0x101] = (char)0xe0;
	xstep();
	chk("shl by 0 leaves value", xm.r[R_AX] & 0xffff, 0x1234);
	chk("shl by 0 leaves cf", (i86flags(&xm) & F_CF) != 0, 1);
	/* SAR of a negative keeps its sign; SHR of the same does not */
	xsetup();
	xm.r[R_AX] = 0xff00; xm.r[R_CX] = 4;
	xseg[0x100] = (char)0xd3; xseg[0x101] = (char)0xf8;	/* sar ax,cl */
	xstep();
	chk("sar keeps sign", xm.r[R_AX] & 0xffff, 0xfff0);
	xsetup();
	xm.r[R_AX] = 0xff00; xm.r[R_CX] = 4;
	xseg[0x100] = (char)0xd3; xseg[0x101] = (char)0xe8;
	xstep();
	chk("shr does not", xm.r[R_AX] & 0xffff, 0x0ff0);
	/* RCR moves the carry in at the top */
	xsetup();
	xm.r[R_AX] = 0x0000;
	xm.fl = (i16)(F_ONES | F_CF);
	xseg[0x100] = (char)0xd1; xseg[0x101] = (char)0xd8;	/* rcr ax,1 */
	xstep();
	chk("rcr brings cf in", xm.r[R_AX] & 0xffff, 0x8000);
	chk("rcr sends 0 out", (i86flags(&xm) & F_CF) != 0, 0);

	/* --- MUL and DIV, both widths --- */
	xsetup();
	xm.r[R_AX] = 0x0102; xm.r[R_BX] = 0x0304;
	xseg[0x100] = (char)0xf7; xseg[0x101] = (char)0xe3;	/* mul bx */
	xstep();
	chk("mul bx ax", xm.r[R_AX] & 0xffff, 0x0a08);
	chk("mul bx dx", xm.r[R_DX] & 0xffff, 0x0003);
	chk("mul bx cf", (i86flags(&xm) & F_CF) != 0, 1);
	xsetup();
	xm.r[R_AX] = 0x000a; xm.r[R_BX] = 0x0003;
	xseg[0x100] = (char)0xf6; xseg[0x101] = (char)0xe3;	/* mul bl */
	xstep();
	chk("mul bl ax", xm.r[R_AX] & 0xffff, 0x001e);
	chk("mul bl cf", (i86flags(&xm) & F_CF) != 0, 0);
	xsetup();
	xm.r[R_AX] = 100; xm.r[R_DX] = 0; xm.r[R_BX] = 7;
	xseg[0x100] = (char)0xf7; xseg[0x101] = (char)0xf3;	/* div bx */
	xstep();
	chk("div bx quot", xm.r[R_AX] & 0xffff, 14);
	chk("div bx rem", xm.r[R_DX] & 0xffff, 2);
	xsetup();
	xm.r[R_AX] = (i16)-100; xm.r[R_DX] = (i16)0xffff; xm.r[R_BX] = 7;
	xseg[0x100] = (char)0xf7; xseg[0x101] = (char)0xfb;	/* idiv bx */
	xstep();
	chk("idiv bx quot", (short)xm.r[R_AX], -14);
	chk("idiv bx rem", (short)xm.r[R_DX], -2);
	/* divide by zero is INT 0, and IP must be back at the divide */
	xsetup();
	xm.r[R_BX] = 0;
	xseg[0x100] = (char)0xf7; xseg[0x101] = (char)0xf3;
	chk("div0 rc", xstep(), X_INT);
	chk("div0 vector", i86intno, 0);
	chk("div0 ip restored", xm.ip & 0xffff, 0x100);

	/* --- CBW and CWD --- */
	xsetup();
	xm.r[R_AX] = 0x00f0;
	xseg[0x100] = (char)0x98;
	chk("cbw negative", (xstep(), xm.r[R_AX] & 0xffff), 0xfff0);
	xsetup();
	xm.r[R_AX] = 0x0070;
	xseg[0x100] = (char)0x98;
	chk("cbw positive", (xstep(), xm.r[R_AX] & 0xffff), 0x0070);
	xsetup();
	xm.r[R_AX] = 0x8000;
	xseg[0x100] = (char)0x99;
	chk("cwd negative", (xstep(), xm.r[R_DX] & 0xffff), 0xffff);

	/* --- PUSHF/POPF round-trip, including the always-one bits --- */
	xsetup();
	xm.r[R_AX] = 0xffff;
	xseg[0x100] = 0x04; xseg[0x101] = 0x01;		/* add al,1 -> CF */
	xseg[0x102] = (char)0x9c;			/* pushf	*/
	xstep(); xstep();
	chk("pushf ones", ((xseg[0xfefe] & 0xff)
			 | ((xseg[0xfeff] & 0xff) << 8)) & F_ONES, F_ONES);
	chk("pushf cf", (((xseg[0xfefe] & 0xff)) & F_CF) != 0, 1);
	xseg[0x103] = (char)0x9d;			/* popf		*/
	xstep();
	chk("popf cf", (i86flags(&xm) & F_CF) != 0, 1);
	chk("popf sp", xm.r[R_SP] & 0xffff, 0xff00);

	/* --- LAHF/SAHF move the low byte only --- */
	xsetup();
	xm.fl = (i16)(F_ONES | F_CF | F_ZF);
	xseg[0x100] = (char)0x9f;			/* lahf		*/
	xstep();
	chk("lahf ah", ((xm.r[R_AX] >> 8) & 0xd5), (F_CF | F_ZF));

	/* --- CLC/STC/CMC/CLD/STD --- */
	xsetup();
	xseg[0x100] = (char)0xf9;			/* stc		*/
	xseg[0x101] = (char)0xf5;			/* cmc		*/
	xstep();
	chk("stc", (i86flags(&xm) & F_CF) != 0, 1);
	xstep();
	chk("cmc", (i86flags(&xm) & F_CF) != 0, 0);
	xsetup();
	xseg[0x100] = (char)0xfd;			/* std		*/
	xstep();
	chk("std", (i86flags(&xm) & F_DF) != 0, 1);

	/* --- XLAT --- */
	xsetup();
	xm.r[R_BX] = 0x2000; xm.r[R_AX] = 0x0003;
	xseg[0x2003] = 0x5a;
	xseg[0x100] = (char)0xd7;
	chk("xlat", (xstep(), xm.r[R_AX] & 0xff), 0x5a);

	/* --- INT leaves the vector and stops --- */
	xsetup();
	xseg[0x100] = (char)0xcd; xseg[0x101] = (char)0xe0;
	chk("int e0 rc", xstep(), X_INT);
	chk("int e0 vector", i86intno, 0xe0);
	chk("int e0 ip past", xm.ip & 0xffff, 0x102);

	/* --- HLT stops without advancing --- */
	xsetup();
	xseg[0x100] = (char)0xf4;
	chk("hlt rc", xstep(), X_HALT);
	chk("hlt ip", xm.ip & 0xffff, 0x100);

	/* --- an unimplemented but real instruction refuses, and does
	 * not advance IP: a refusal has to be able to name its own
	 * address, which is the whole reason the decoder is complete --- */
	xsetup();
	xseg[0x100] = (char)0xe4; xseg[0x101] = 0x00;	/* in al,0	*/
	chk("in refused", xstep(), X_UNIMP);
	chk("in ip", xm.ip & 0xffff, 0x100);
	xsetup();
	xseg[0x100] = (char)0xd6;			/* SALC		*/
	chk("salc is not an instruction", xstep(), X_BAD);
	/* INTO stays refused: an overflow trap is a vector nothing here
	 * claims.  IRET does not -- see section 3c. */
	xsetup();
	xseg[0x100] = (char)0xce;			/* into		*/
	chk("into refused", xstep(), X_UNIMP);
	chk("into ip", xm.ip & 0xffff, 0x100);
}

/* ================================================================== */
/* 3a. string operations					      */
/* ================================================================== */

static void xsetup2(void);		/* section 3b, below		*/

/* Lay `n' bytes of `s' at xseg[off]. */
static void put(int off, const char *s, int n)
{
	memcpy(&xseg[off], s, (size_t)n);
}

static void t_string(void)
{
	/* --- MOVSB, forward, no prefix --- */
	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	xseg[0x2000] = 0x5a;
	xseg[0x100] = (char)0xa4;
	chk("movsb rc", xstep(), X_OK);
	chk("movsb byte", xseg[0x3000] & 0xff, 0x5a);
	chk("movsb si", xm.r[R_SI] & 0xffff, 0x2001);
	chk("movsb di", xm.r[R_DI] & 0xffff, 0x3001);
	chk("movsb ip", xm.ip & 0xffff, 0x101);

	/* --- MOVSB with DF set walks backwards --- */
	xsetup();
	xm.fl |= F_DF;
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	xseg[0x2000] = 0x77;
	xseg[0x100] = (char)0xa4;
	chk("movsb df rc", xstep(), X_OK);
	chk("movsb df byte", xseg[0x3000] & 0xff, 0x77);
	chk("movsb df si", xm.r[R_SI] & 0xffff, 0x1fff);
	chk("movsb df di", xm.r[R_DI] & 0xffff, 0x2fff);

	/* --- MOVSW moves two bytes and steps by two, each way --- */
	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	put(0x2000, "\x34\x12", 2);
	xseg[0x100] = (char)0xa5;
	chk("movsw rc", xstep(), X_OK);
	chk("movsw lo", xseg[0x3000] & 0xff, 0x34);
	chk("movsw hi", xseg[0x3001] & 0xff, 0x12);
	chk("movsw si", xm.r[R_SI] & 0xffff, 0x2002);
	chk("movsw di", xm.r[R_DI] & 0xffff, 0x3002);

	xsetup();
	xm.fl |= F_DF;
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	xseg[0x100] = (char)0xa5;
	chk("movsw df si", (xstep(), xm.r[R_SI] & 0xffff), 0x1ffe);
	chk("movsw df di", xm.r[R_DI] & 0xffff, 0x2ffe);

	/* --- REP MOVSB copies CX bytes and leaves CX zero --- */
	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 4;
	put(0x2000, "abcd", 4);
	xseg[0x100] = (char)0xf3; xseg[0x101] = (char)0xa4;
	chk("rep movsb rc", xstep(), X_OK);
	chk("rep movsb copied", memcmp(&xseg[0x3000], "abcd", 4), 0);
	chk("rep movsb stopped", xseg[0x3004] & 0xff, 0);
	chk("rep movsb cx", xm.r[R_CX] & 0xffff, 0);
	chk("rep movsb si", xm.r[R_SI] & 0xffff, 0x2004);
	chk("rep movsb di", xm.r[R_DI] & 0xffff, 0x3004);
	chk("rep movsb ip", xm.ip & 0xffff, 0x102);

	/* --- REP MOVSW counts WORDS, not bytes --- */
	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 3;
	put(0x2000, "ABCDEF", 6);
	xseg[0x100] = (char)0xf3; xseg[0x101] = (char)0xa5;
	chk("rep movsw rc", xstep(), X_OK);
	chk("rep movsw copied", memcmp(&xseg[0x3000], "ABCDEF", 6), 0);
	chk("rep movsw stopped", xseg[0x3006] & 0xff, 0);
	chk("rep movsw cx", xm.r[R_CX] & 0xffff, 0);

	/* --- REP with CX = 0 does nothing and falls through --- */
	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 0;
	xseg[0x2000] = 0x11;
	xseg[0x100] = (char)0xf3; xseg[0x101] = (char)0xa4;
	chk("rep cx0 rc", xstep(), X_OK);
	chk("rep cx0 wrote nothing", xseg[0x3000] & 0xff, 0);
	chk("rep cx0 si", xm.r[R_SI] & 0xffff, 0x2000);
	chk("rep cx0 di", xm.r[R_DI] & 0xffff, 0x3000);
	chk("rep cx0 ip", xm.ip & 0xffff, 0x102);

	/* --- STOS, both sizes; the byte form must not touch AH --- */
	xsetup();
	xm.r[R_AX] = 0xbe5a; xm.r[R_DI] = 0x3000;
	xseg[0x100] = (char)0xaa;
	chk("stosb rc", xstep(), X_OK);
	chk("stosb byte", xseg[0x3000] & 0xff, 0x5a);
	chk("stosb untouched", xseg[0x3001] & 0xff, 0);
	chk("stosb di", xm.r[R_DI] & 0xffff, 0x3001);

	xsetup();
	xm.r[R_AX] = 0x1234; xm.r[R_DI] = 0x3000;
	xseg[0x100] = (char)0xab;
	chk("stosw rc", xstep(), X_OK);
	chk("stosw lo", xseg[0x3000] & 0xff, 0x34);
	chk("stosw hi", xseg[0x3001] & 0xff, 0x12);
	chk("stosw di", xm.r[R_DI] & 0xffff, 0x3002);

	xsetup();
	xm.fl |= F_DF;
	xm.r[R_AX] = 0x00ff; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 3;
	xseg[0x100] = (char)0xf3; xseg[0x101] = (char)0xaa;
	chk("rep stosb df rc", xstep(), X_OK);
	chk("rep stosb df filled", memcmp(&xseg[0x2ffe], "\xff\xff\xff", 3),
		0);
	chk("rep stosb df above", xseg[0x3001] & 0xff, 0);
	chk("rep stosb df di", xm.r[R_DI] & 0xffff, 0x2ffd);
	chk("rep stosb df cx", xm.r[R_CX] & 0xffff, 0);

	/* --- LODS, both sizes --- */
	xsetup();
	xm.r[R_AX] = 0xbeef; xm.r[R_SI] = 0x2000;
	xseg[0x2000] = 0x5a;
	xseg[0x100] = (char)0xac;
	chk("lodsb rc", xstep(), X_OK);
	chk("lodsb ax", xm.r[R_AX] & 0xffff, 0xbe5a);
	chk("lodsb si", xm.r[R_SI] & 0xffff, 0x2001);

	xsetup();
	xm.r[R_SI] = 0x2000;
	put(0x2000, "\x78\x56", 2);
	xseg[0x100] = (char)0xad;
	chk("lodsw ax", (xstep(), xm.r[R_AX] & 0xffff), 0x5678);
	chk("lodsw si", xm.r[R_SI] & 0xffff, 0x2002);

	/* --- MOVS, STOS and LODS leave the flags exactly as they were --- */
	xsetup();
	xm.fl = (i16)(F_ONES | F_CF | F_ZF | F_SF);
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 2;
	xseg[0x100] = (char)0xf3; xseg[0x101] = (char)0xa4;
	xstep();
	chk("rep movsb keeps flags", i86flags(&xm) & 0xffff,
		(long)(F_ONES | F_CF | F_ZF | F_SF));

	/* --- CMPS sets the flags a CMP would, and stores nothing --- */
	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	xseg[0x2000] = 0x42; xseg[0x3000] = 0x42;
	xseg[0x100] = (char)0xa6;
	chk("cmpsb equal rc", xstep(), X_OK);
	chk("cmpsb equal zf", (i86flags(&xm) & F_ZF) != 0, 1);
	chk("cmpsb equal cf", (xm.fl & F_CF) != 0, 0);
	chk("cmpsb equal si", xm.r[R_SI] & 0xffff, 0x2001);
	chk("cmpsb equal di", xm.r[R_DI] & 0xffff, 0x3001);

	/* [SI] - [DI], so a smaller source borrows. */
	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	xseg[0x2000] = 0x10; xseg[0x3000] = 0x20;
	xseg[0x100] = (char)0xa6;
	xstep();
	chk("cmpsb less zf", (i86flags(&xm) & F_ZF) != 0, 0);
	chk("cmpsb less cf", (xm.fl & F_CF) != 0, 1);

	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	put(0x2000, "\x00\x80", 2);
	put(0x3000, "\x00\x80", 2);
	xseg[0x100] = (char)0xa7;
	chk("cmpsw equal rc", xstep(), X_OK);
	chk("cmpsw equal zf", (i86flags(&xm) & F_ZF) != 0, 1);
	chk("cmpsw si", xm.r[R_SI] & 0xffff, 0x2002);

	/* --- REPE CMPSB stops at the first difference --- */
	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 4;
	put(0x2000, "abcd", 4);
	put(0x3000, "abxd", 4);
	xseg[0x100] = (char)0xf3; xseg[0x101] = (char)0xa6;
	chk("repe cmpsb rc", xstep(), X_OK);
	chk("repe cmpsb cx", xm.r[R_CX] & 0xffff, 1);
	chk("repe cmpsb si", xm.r[R_SI] & 0xffff, 0x2003);
	chk("repe cmpsb di", xm.r[R_DI] & 0xffff, 0x3003);
	chk("repe cmpsb zf", (i86flags(&xm) & F_ZF) != 0, 0);

	/* Equal all the way through: the count runs out instead. */
	xsetup();
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 4;
	put(0x2000, "abcd", 4);
	put(0x3000, "abcd", 4);
	xseg[0x100] = (char)0xf3; xseg[0x101] = (char)0xa6;
	xstep();
	chk("repe cmpsb all cx", xm.r[R_CX] & 0xffff, 0);
	chk("repe cmpsb all zf", (i86flags(&xm) & F_ZF) != 0, 1);

	/* --- SCAS, and REPNE stopping on the match --- */
	xsetup();
	xm.r[R_AX] = 0x0033; xm.r[R_DI] = 0x3000;
	xseg[0x3000] = 0x33;
	xseg[0x100] = (char)0xae;
	chk("scasb rc", xstep(), X_OK);
	chk("scasb zf", (i86flags(&xm) & F_ZF) != 0, 1);
	chk("scasb di", xm.r[R_DI] & 0xffff, 0x3001);

	xsetup();
	xm.r[R_AX] = 0x0033; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 4;
	put(0x3000, "\x11\x22\x33\x44", 4);
	xseg[0x100] = (char)0xf2; xseg[0x101] = (char)0xae;
	chk("repne scasb rc", xstep(), X_OK);
	chk("repne scasb cx", xm.r[R_CX] & 0xffff, 1);
	chk("repne scasb di", xm.r[R_DI] & 0xffff, 0x3003);
	chk("repne scasb zf", (i86flags(&xm) & F_ZF) != 0, 1);

	/* No match anywhere: the count runs out and ZF is clear. */
	xsetup();
	xm.r[R_AX] = 0x0099; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 4;
	put(0x3000, "\x11\x22\x33\x44", 4);
	xseg[0x100] = (char)0xf2; xseg[0x101] = (char)0xae;
	xstep();
	chk("repne scasb miss cx", xm.r[R_CX] & 0xffff, 0);
	chk("repne scasb miss di", xm.r[R_DI] & 0xffff, 0x3004);
	chk("repne scasb miss zf", (i86flags(&xm) & F_ZF) != 0, 0);

	/* REPE SCASW scans while the words match. */
	xsetup();
	xm.r[R_AX] = 0x1111; xm.r[R_DI] = 0x3000; xm.r[R_CX] = 4;
	put(0x3000, "\x11\x11\x11\x11\x22\x22", 6);
	xseg[0x100] = (char)0xf3; xseg[0x101] = (char)0xaf;
	chk("repe scasw rc", xstep(), X_OK);
	chk("repe scasw cx", xm.r[R_CX] & 0xffff, 1);
	chk("repe scasw di", xm.r[R_DI] & 0xffff, 0x3006);

	/* --- the 64 KB boundary: SI and DI wrap, they do not run on --- */
	xsetup();
	xm.r[R_AX] = 0x00aa; xm.r[R_DI] = 0xffff; xm.r[R_CX] = 2;
	xseg[0x100] = (char)0xf3; xseg[0x101] = (char)0xaa;
	chk("rep stosb wrap rc", xstep(), X_OK);
	chk("rep stosb wrap top", xseg[0xffff] & 0xff, 0xaa);
	chk("rep stosb wrap round", xseg[0x0000] & 0xff, 0xaa);
	chk("rep stosb wrap di", xm.r[R_DI] & 0xffff, 1);

	/* A word straddling the top: low byte at 0xFFFF, high byte at 0. */
	xsetup();
	xm.r[R_AX] = 0x1234; xm.r[R_DI] = 0xffff;
	xseg[0x100] = (char)0xab;
	chk("stosw wrap rc", xstep(), X_OK);
	chk("stosw wrap lo", xseg[0xffff] & 0xff, 0x34);
	chk("stosw wrap hi", xseg[0x0000] & 0xff, 0x12);
	chk("stosw wrap di", xm.r[R_DI] & 0xffff, 1);

	xsetup();
	xm.fl |= F_DF;
	xm.r[R_SI] = 0x0000; xm.r[R_DI] = 0x3000;
	xseg[0x0000] = 0x6b;
	xseg[0x100] = (char)0xa4;
	chk("movsb wrap down rc", xstep(), X_OK);
	chk("movsb wrap down byte", xseg[0x3000] & 0xff, 0x6b);
	chk("movsb wrap down si", xm.r[R_SI] & 0xffff, 0xffff);

	/* --- the segment override moves the SOURCE and only the source.
	 * ES is the second segment here, so a plain MOVSB reads DS and
	 * writes ES, and SS: MOVSB reads the ES segment instead. --- */
	xsetup2();
	xm.sr[S_ES] = 0x4000; xm.sb[S_ES] = xseg2;
	xm.sr[S_SS] = 0x4000; xm.sb[S_SS] = xseg2;
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	xseg[0x2000] = 0x11; xseg2[0x2000] = 0x22;
	xseg[0x100] = (char)0xa4;
	chk("movsb ds rc", xstep(), X_OK);
	chk("movsb ds source", xseg2[0x3000] & 0xff, 0x11);

	xsetup2();
	xm.sr[S_ES] = 0x4000; xm.sb[S_ES] = xseg2;
	xm.sr[S_SS] = 0x4000; xm.sb[S_SS] = xseg2;
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	xseg[0x2000] = 0x11; xseg2[0x2000] = 0x22;
	xseg[0x100] = (char)0x36; xseg[0x101] = (char)0xa4;	/* ss: */
	chk("movsb ss: rc", xstep(), X_OK);
	chk("movsb ss: source", xseg2[0x3000] & 0xff, 0x22);
	chk("movsb ss: dest still es", xseg[0x3000] & 0xff, 0);

	/* CMPS reads its destination through ES whatever the prefix says. */
	xsetup2();
	xm.sr[S_ES] = 0x4000; xm.sb[S_ES] = xseg2;
	xm.r[R_SI] = 0x2000; xm.r[R_DI] = 0x3000;
	xseg[0x2000] = 0x44; xseg2[0x3000] = 0x44; xseg[0x3000] = 0x55;
	xseg[0x100] = (char)0xa6;
	xstep();
	chk("cmpsb dest is es", (i86flags(&xm) & F_ZF) != 0, 1);

	/* --- a biased ES window: the write that leaves it refuses, and
	 * does not wrap round to the bottom of the host segment --- */
	xsetup();
	i86nseg = 2;
	i86spar[1] = 0x2000; i86sbase[1] = xseg2;
	memset(xseg2, 0, sizeof xseg2);
	i86nsegslow = i86nsegbad = 0;
	xm.r[R_CX] = 0x2010;
	xseg[0x100] = (char)0x8e; xseg[0x101] = (char)0xc1;	/* mov es,cx */
	chk("es slow rc", xstep(), X_OK);
	chk("es bias", xm.so[S_ES] & 0xffff, 0x100);

	xm.r[R_AX] = 0x005a; xm.r[R_DI] = 0xfff0; xm.r[R_CX] = 1;
	xseg[0x102] = (char)0xf3; xseg[0x103] = (char)0xaa;
	chk("rep stosb window rc", xstep(), X_WINDOW);
	chk("rep stosb window ip", xm.ip & 0xffff, 0x102);
	chk("rep stosb window slot", xm.fseg, S_ES);
	chk("rep stosb window off", xm.foff & 0xffff, 0xfff0);
	chk("rep stosb window no wrap", xseg2[0xf0] & 0xff, 0);
	i86nsegslow = i86nsegbad = 0;
}

/* ================================================================== */
/* 3b. far transfers, and the warm boot that sits in front of them    */
/* ================================================================== */

/*
 * WHY THIS SECTION EXISTS AT ALL.  Stage one refused every far transfer,
 * and the refusal was correct but it was a ceiling: DRI's SUBMIT.CMD
 * ends on one and could not finish.  Two things changed and they are
 * tested apart from each other, because they are different KINDS of
 * claim:
 *
 *   - the mechanism.  A far transfer loads CS, and loading CS is the
 *     same act as loading any other segment register, so it goes
 *     through setsr() and inherits its refusal.  That is an 8086 claim
 *     and the tests below are 8086 tests.
 *   - the rule.  A far transfer to <entry SS>:0000 is this
 *     ENVIRONMENT's warm boot.  That is not an 8086 claim -- the
 *     hardware has no such thing -- and the tests for it are about
 *     which transfers it does and does not catch.
 *
 * The order matters and is tested: the entry stack segment is a
 * paragraph the shim DID hand out, so setsr() would resolve it happily
 * and drop the guest into its own base page.  The rule has to be asked
 * first, and "far_wboot beats a resolvable segment" is the check that
 * says it is.
 */

/* xsetup() with a SECOND paragraph, 0x4000, so a far transfer has
 * somewhere real to land and an unhanded paragraph is still unhanded. */
static void xsetup2(void)
{
	xsetup();
	memset(xseg2, 0, sizeof xseg2);
	i86nseg = 2;
	i86spar[1] = 0x4000;
	i86sbase[1] = xseg2;
}

static void t_far(void)
{
	/* --- JMPF: EA off16 seg16 --- */
	xsetup2();
	xseg[0x100] = (char)0xea;
	xseg[0x101] = 0x00; xseg[0x102] = 0x02;		/* offset 0200	*/
	xseg[0x103] = 0x00; xseg[0x104] = 0x40;		/* segment 4000	*/
	chk("jmpf rc", xstep(), X_OK);
	chk("jmpf cs", xm.sr[S_CS] & 0xffff, 0x4000);
	chk("jmpf ip", xm.ip & 0xffff, 0x0200);
	chk("jmpf base rebound", (long)(xm.sb[S_CS] == xseg2), 1);
	chk("jmpf no bias", xm.so[S_CS] & 0xffff, 0);
	chk("jmpf no slow path", (long)i86nsegslow, 0);

	/* --- JMPF into a paragraph we never handed out: X_SEGESC, and
	 * NOTHING moves.  This is the check that makes the whole feature
	 * bounded -- a far transfer cannot reach memory the shim does not
	 * own, for exactly the reason `MOV ES,ax' cannot. --- */
	xsetup2();
	i86nsegbad = 0;
	xseg[0x100] = (char)0xea;
	xseg[0x101] = 0x00; xseg[0x102] = 0x00;
	xseg[0x103] = 0x00; xseg[0x104] = (char)0xd0;	/* segment D000	*/
	chk("jmpf stranger rc", xstep(), X_SEGESC);
	chk("jmpf stranger ip", xm.ip & 0xffff, 0x100);
	chk("jmpf stranger cs", xm.sr[S_CS] & 0xffff, 0x1000);
	chk("jmpf stranger base", (long)(xm.sb[S_CS] == xseg), 1);
	chk("jmpf stranger named", i86segbad & 0xffff, 0xd000);
	chk("jmpf stranger counted", (long)i86nsegbad, 1);

	/* --- CALLF: 9A off16 seg16, and the frame it leaves --- */
	xsetup2();
	xseg[0x100] = (char)0x9a;
	xseg[0x101] = 0x34; xseg[0x102] = 0x12;
	xseg[0x103] = 0x00; xseg[0x104] = 0x40;
	chk("callf rc", xstep(), X_OK);
	chk("callf cs", xm.sr[S_CS] & 0xffff, 0x4000);
	chk("callf ip", xm.ip & 0xffff, 0x1234);
	chk("callf sp", xm.r[R_SP] & 0xffff, 0xfefc);
	/* SS is still the first segment, so the frame is in xseg: return
	 * offset 0x105 at the lower address, return segment above it. */
	chk("callf pushed ip", ((xseg[0xfefc] & 0xff)
			      | ((xseg[0xfefd] & 0xff) << 8)), 0x0105);
	chk("callf pushed cs", ((xseg[0xfefe] & 0xff)
			      | ((xseg[0xfeff] & 0xff) << 8)), 0x1000);

	/* --- CALLF that cannot resolve leaves the STACK alone too.  The
	 * segment is loaded before anything is pushed for this reason. --- */
	xsetup2();
	xseg[0x100] = (char)0x9a;
	xseg[0x101] = 0x34; xseg[0x102] = 0x12;
	xseg[0x103] = 0x00; xseg[0x104] = (char)0xd0;
	chk("callf stranger rc", xstep(), X_SEGESC);
	chk("callf stranger sp", xm.r[R_SP] & 0xffff, 0xff00);
	chk("callf stranger ip", xm.ip & 0xffff, 0x100);

	/* --- RETF, over a frame CALLF could have left --- */
	xsetup2();
	xm.r[R_SP] = 0xfefc;
	xseg[0xfefc] = 0x34; xseg[0xfefd] = 0x12;	/* offset 1234	*/
	xseg[0xfefe] = 0x00; xseg[0xfeff] = 0x40;	/* segment 4000	*/
	xseg[0x100] = (char)0xcb;
	chk("retf rc", xstep(), X_OK);
	chk("retf cs", xm.sr[S_CS] & 0xffff, 0x4000);
	chk("retf ip", xm.ip & 0xffff, 0x1234);
	chk("retf sp", xm.r[R_SP] & 0xffff, 0xff00);

	/* --- RETF imm16 pops the arguments as well --- */
	xsetup2();
	xm.r[R_SP] = 0xfefc;
	xseg[0xfefc] = 0x00; xseg[0xfefd] = 0x03;
	xseg[0xfefe] = 0x00; xseg[0xfeff] = 0x40;
	xseg[0x100] = (char)0xca; xseg[0x101] = 0x04; xseg[0x102] = 0x00;
	chk("retf imm rc", xstep(), X_OK);
	chk("retf imm ip", xm.ip & 0xffff, 0x0300);
	chk("retf imm sp", xm.r[R_SP] & 0xffff, 0xff04);

	/* --- RETF over a frame naming a paragraph we do not hold: SP does
	 * not move, so the guest is exactly where the instruction found
	 * it and the refusal can be believed. --- */
	xsetup2();
	xm.r[R_SP] = 0xfefc;
	xseg[0xfefe] = 0x00; xseg[0xfeff] = (char)0xd0;
	xseg[0x100] = (char)0xcb;
	chk("retf stranger rc", xstep(), X_SEGESC);
	chk("retf stranger sp", xm.r[R_SP] & 0xffff, 0xfefc);
	chk("retf stranger ip", xm.ip & 0xffff, 0x100);

	/* --- FF /5: the far indirect jump, the form SUBMIT.CMD ends on.
	 * The dword at the effective address is offset then segment. --- */
	xsetup2();
	xseg[0x2000] = 0x50; xseg[0x2001] = 0x00;	/* offset 0050	*/
	xseg[0x2002] = 0x00; xseg[0x2003] = 0x40;	/* segment 4000	*/
	xseg[0x100] = (char)0xff; xseg[0x101] = 0x2e;
	xseg[0x102] = 0x00; xseg[0x103] = 0x20;
	chk("jmpi far rc", xstep(), X_OK);
	chk("jmpi far cs", xm.sr[S_CS] & 0xffff, 0x4000);
	chk("jmpi far ip", xm.ip & 0xffff, 0x0050);

	/* --- FF /3: the far indirect call --- */
	xsetup2();
	xseg[0x2000] = 0x60; xseg[0x2001] = 0x00;
	xseg[0x2002] = 0x00; xseg[0x2003] = 0x40;
	xseg[0x100] = (char)0xff; xseg[0x101] = 0x1e;
	xseg[0x102] = 0x00; xseg[0x103] = 0x20;
	chk("calli far rc", xstep(), X_OK);
	chk("calli far cs", xm.sr[S_CS] & 0xffff, 0x4000);
	chk("calli far ip", xm.ip & 0xffff, 0x0060);
	chk("calli far pushed ip", ((xseg[0xfefc] & 0xff)
				  | ((xseg[0xfefd] & 0xff) << 8)), 0x0104);
	chk("calli far pushed cs", ((xseg[0xfefe] & 0xff)
				  | ((xseg[0xfeff] & 0xff) << 8)), 0x1000);

	/* --- a far JMP whose mod r/m names a REGISTER is not an 8086
	 * instruction, and is refused as one rather than executed as the
	 * near form --- */
	xsetup2();
	xseg[0x100] = (char)0xff; xseg[0x101] = (char)0xe8;	/* mod=3 */
	chk("far jmp of a register", xstep(), X_BAD);

	/* ---------------------------------------------------------------
	 * The rule.  From here on the machine has an entry stack segment
	 * recorded, which is what i86place() does for a real guest.
	 * ------------------------------------------------------------- */

	/* --- SUBMIT's own shape: CS: JMP FAR [0059] where the word pair
	 * at CS:0059 is 0000 and the entry SS.  X_WBOOT, and IP left AT
	 * the jump so a transcript can name it. --- */
	xsetup2();
	xm.wset = 1; xm.wseg = 0x1000;
	xseg[0x0059] = 0x00; xseg[0x005a] = 0x00;	/* offset 0000	*/
	xseg[0x005b] = 0x00; xseg[0x005c] = 0x10;	/* segment 1000	*/
	xseg[0x100] = 0x2e;				/* CS: prefix	*/
	xseg[0x101] = (char)0xff; xseg[0x102] = 0x2e;
	xseg[0x103] = 0x59; xseg[0x104] = 0x00;
	chk("far wboot rc", xstep(), X_WBOOT);
	chk("far wboot ip", xm.ip & 0xffff, 0x100);
	chk("far wboot cs", xm.sr[S_CS] & 0xffff, 0x1000);

	/* --- and it beats the mechanism.  0x1000 IS a paragraph the shim
	 * handed out, so setsr() would have taken it and the guest would
	 * have carried on executing its own base page.  This check is the
	 * ordering claim: the rule is asked first. --- */
	chk("far wboot beats a resolvable segment",
		(long)(i86resolve((i16)0x1000) == xseg), 1);

	/* --- the same target reached by JMPF and by RETF: the rule is
	 * about the TARGET, not about one encoding --- */
	xsetup2();
	xm.wset = 1; xm.wseg = 0x1000;
	xseg[0x100] = (char)0xea;
	xseg[0x101] = 0x00; xseg[0x102] = 0x00;
	xseg[0x103] = 0x00; xseg[0x104] = 0x10;
	chk("jmpf wboot", xstep(), X_WBOOT);
	xsetup2();
	xm.wset = 1; xm.wseg = 0x1000;
	xm.r[R_SP] = 0xfefc;
	xseg[0xfefe] = 0x00; xseg[0xfeff] = 0x10;
	xseg[0x100] = (char)0xcb;
	chk("retf wboot", xstep(), X_WBOOT);
	chk("retf wboot sp", xm.r[R_SP] & 0xffff, 0xfefc);

	/* --- HOW NARROW IT IS.  Offset 1 of the same segment is an
	 * ordinary far jump; offset 0 of a DIFFERENT segment is an
	 * ordinary far jump; and a NEAR jump to offset 0 is not a warm
	 * boot at all, because on a real CP/M-86 machine an 8080-model
	 * program's `JMP 0' lands in its own base page and is a bug. --- */
	xsetup2();
	xm.wset = 1; xm.wseg = 0x1000;
	xseg[0x100] = (char)0xea;
	xseg[0x101] = 0x01; xseg[0x102] = 0x00;		/* offset 0001	*/
	xseg[0x103] = 0x00; xseg[0x104] = 0x10;
	chk("wboot needs offset 0", xstep(), X_OK);
	chk("wboot needs offset 0 ip", xm.ip & 0xffff, 0x0001);
	xsetup2();
	xm.wset = 1; xm.wseg = 0x1000;
	xseg[0x100] = (char)0xea;
	xseg[0x101] = 0x00; xseg[0x102] = 0x00;
	xseg[0x103] = 0x00; xseg[0x104] = 0x40;		/* segment 4000	*/
	chk("wboot needs the entry SS", xstep(), X_OK);
	chk("wboot needs the entry SS cs", xm.sr[S_CS] & 0xffff, 0x4000);
	xsetup2();
	xm.wset = 1; xm.wseg = 0x1000;
	xseg[0x100] = (char)0xe9;			/* near jmp 0	*/
	xseg[0x101] = (char)0xfd; xseg[0x102] = (char)0xfe;
	chk("near jmp 0 is not a warm boot", xstep(), X_OK);
	chk("near jmp 0 ip", xm.ip & 0xffff, 0);

	/* --- and it is OFF until a loader turns it on.  A machine nobody
	 * placed a program into has no entry stack segment, and paragraph
	 * 0 must not become a magic address by accident. --- */
	xsetup2();
	xseg[0x100] = (char)0xea;
	xseg[0x101] = 0x00; xseg[0x102] = 0x00;
	xseg[0x103] = 0x00; xseg[0x104] = 0x10;
	chk("no wboot without a loader", xstep(), X_OK);
	chk("no wboot without a loader ip", xm.ip & 0xffff, 0);

	/* Hand K3's counters back the way this section found them.  They
	 * are global instruments, section 4 asserts absolute values on
	 * them, and the refusals above are this section's own traffic. */
	i86nsegslow = i86nsegbad = 0;
	i86nseg = 1;
}

/*
 * The segment-register check, kill criterion K3.  Writing a paragraph
 * the shim handed out succeeds and rebinds the host base; writing one
 * it did not is X_SEGESC, the counter moves, and -- this is the part
 * that matters -- NOTHING ELSE CHANGES.  A partial write here is the
 * silent-data-corruption failure the plan says must not exist.
 */
static char oseg[65536];

/* The i86segnew hook, for the one test that needs it: it answers with a
 * segment of its own for any paragraph at all, which is the policy a
 * target build with a spare 64 KB page could adopt. */
static i16 hookpar;

static char *hookseg(par)
i16 par;
{
	hookpar = par;
	return (oseg);
}

static void t_segcheck(void)
{
	int rc;

	xsetup();
	i86nseg = 2;
	i86spar[0] = 0x1000; i86sbase[0] = xseg;
	i86spar[1] = 0x2000; i86sbase[1] = oseg;
	i86nsegslow = 0;

	xm.r[R_CX] = 0x2000;
	xseg[0x100] = (char)0x8e; xseg[0x101] = (char)0xc1;	/* mov es,cx */
	chk("assigned seg rc", xstep(), X_OK);
	chk("assigned seg par", xm.sr[S_ES] & 0xffff, 0x2000);
	ntest++;
	if (xm.sb[S_ES] != oseg)
		fail("assigned seg base", 1, 0);
	chk("assigned seg no escape", (long)i86nsegslow, 0);

	xsetup();
	i86nseg = 2;
	i86spar[0] = 0x1000; i86sbase[0] = xseg;
	i86spar[1] = 0x2000; i86sbase[1] = oseg;
	i86nsegslow = 0;
	xm.r[R_CX] = 0xd000;		/* dBASE II's absolute literal	*/
	xseg[0x100] = (char)0x8e; xseg[0x101] = (char)0xc1;
	rc = xstep();
	chk("absolute seg refused", rc, X_SEGESC);
	chk("absolute seg reported", i86segbad & 0xffff, 0xd000);
	chk("absolute seg counted", (long)i86nsegbad, 1);
	chk("absolute seg not slow", (long)i86nsegslow, 0);
	chk("absolute seg unchanged", xm.sr[S_ES] & 0xffff, 0x1000);
	chk("absolute seg ip restored", xm.ip & 0xffff, 0x100);

	/* POP ES of an unassigned paragraph must not consume the stack:
	 * a refusal the guest could be resumed past has to leave the
	 * machine exactly as it found it. */
	xsetup();
	i86nseg = 1;
	i86spar[0] = 0x1000; i86sbase[0] = xseg;
	i86nsegslow = 0;
	xm.r[R_SP] = 0xfefe;
	xseg[0xfefe] = 0x40; xseg[0xfeff] = 0x00;	/* the PC BIOS	*/
	xseg[0x100] = 0x07;				/* pop es	*/
	chk("pop es refused", xstep(), X_SEGESC);
	chk("pop es sp intact", xm.r[R_SP] & 0xffff, 0xfefe);
	chk("pop es reported", i86segbad & 0xffff, 0x0040);

	/* MOV CS,x is not an instruction on any x86; it must refuse
	 * rather than take the segment check's word for it. */
	xsetup();
	xseg[0x100] = (char)0x8e; xseg[0x101] = (char)0xc9;	/* mov cs,cx */
	chk("mov cs refused", xstep(), X_UNIMP);
}

/*
 * E1s, the slow path itself.  The case it exists for is pointer
 * normalisation -- (seg, off) -> (seg + off/16, off & 15) -- which
 * produces a paragraph INSIDE a segment we hold, and which no DRI, no
 * dBASE II and no WordStar binary was ever seen doing
 * (CPM86-SHIM-FEASIBILITY.md §1.3).  So this is a test of code the
 * corpus does not reach, written because the corpus is not the world
 * and because the failure mode without it is silent.
 *
 * Three things have to hold: the biased window addresses the right
 * bytes, a reference that runs off the end of the host segment is a
 * refusal rather than a wrap, and rebinding the slot to a real base
 * clears the bias again.
 */
static void t_segslow(void)
{
	int i;

	xsetup();
	i86nseg = 2;
	i86spar[0] = 0x1000; i86sbase[0] = xseg;
	i86spar[1] = 0x2000; i86sbase[1] = oseg;
	i86nsegslow = i86nsegbad = 0;

	for (i = 0; i < 0x40; i++)
		oseg[0x100 + i] = (char)(0xa0 + i);

	/* DS := 0x2010, ten paragraphs into the segment we handed out at
	 * 0x2000, which is what normalising (0x2000, 0x0100) gives. */
	xm.r[R_CX] = 0x2010;
	xseg[0x100] = (char)0x8e; xseg[0x101] = (char)0xd9;	/* mov ds,cx */
	chk("slow rc", xstep(), X_OK);
	chk("slow counted", (long)i86nsegslow, 1);
	chk("slow not bad", (long)i86nsegbad, 0);
	chk("slow par readback", xm.sr[S_DS] & 0xffff, 0x2010);
	chk("slow bias", xm.so[S_DS] & 0xffff, 0x100);

	/* MOV AL,[0] now reads oseg[0x100], the byte the guest means. */
	xseg[0x102] = (char)0xa0; xseg[0x103] = 0x00; xseg[0x104] = 0x00;
	chk("slow read rc", xstep(), X_OK);
	chk("slow read byte", xm.r[R_AX] & 0xff, 0xa0);

	/* MOV AL,[0xFFF0] is 0x100 + 0xFFF0 = 0x100F0, past the end of a
	 * 64 KB host segment.  The guest means the paragraph after the
	 * segment, which belongs to something else, so this refuses --
	 * and it must not wrap round to oseg[0xF0]. */
	oseg[0xf0] = (char)0x5a;
	xm.r[R_AX] = 0;
	xseg[0x105] = (char)0xa0; xseg[0x106] = (char)0xf0;
	xseg[0x107] = (char)0xff;
	chk("slow window rc", xstep(), X_WINDOW);
	chk("slow window ip restored", xm.ip & 0xffff, 0x105);
	chk("slow window slot", xm.fseg, S_DS);
	chk("slow window off", xm.foff & 0xffff, 0xfff0);
	chk("slow window did not wrap", xm.r[R_AX] & 0xff, 0);

	/* i86addr() is the seam's view of the same window: 16 bytes at
	 * 0xFFF0 do not fit, one byte at 0xFEFF does. */
	ntest++;
	if (i86addr(&xm, S_DS, (i16)0xfff0, 16L) != (char *)0)
		fail("i86addr past end", 1, 0);
	ntest++;
	if (i86addr(&xm, S_DS, (i16)0x0000, 1L) != &oseg[0x100])
		fail("i86addr biased base", 1, 0);
	ntest++;
	if (i86addr(&xm, S_ES, (i16)0xff80, 128L) != &xseg[0xff80])
		fail("i86addr unbiased top", 1, 0);
	ntest++;
	if (i86addr(&xm, S_ES, (i16)0xff81, 128L) != (char *)0)
		fail("i86addr unbiased past top", 1, 0);

	/* Rebinding to a paragraph we did hand out clears the bias. */
	xm.ip = 0x200;
	xm.r[R_CX] = 0x2000;
	xseg[0x200] = (char)0x8e; xseg[0x201] = (char)0xd9;
	chk("rebind rc", xstep(), X_OK);
	chk("rebind bias cleared", xm.so[S_DS] & 0xffff, 0);

	/* A biased CS is refused before a byte is decoded: i86dec()
	 * wraps at the guest's 64 KB and would read past the host
	 * segment's end doing it. */
	xsetup();
	i86nseg = 1;
	i86spar[0] = 0x1000; i86sbase[0] = xseg;
	xm.so[S_CS] = 0x10;
	chk("biased cs refused", xstep(), X_WINDOW);
	chk("biased cs slot", xm.fseg, S_CS);

	/* And the hook: an absolute literal nothing contains becomes
	 * whatever the segment owner says it is.  With no hook it is
	 * X_SEGESC (tested above); with one it is a segment. */
	xsetup();
	i86nseg = 1;
	i86spar[0] = 0x1000; i86sbase[0] = xseg;
	i86nsegslow = i86nsegbad = 0;
	i86segnew = hookseg;
	hookpar = 0;
	xm.r[R_CX] = 0xd000;
	xseg[0x100] = (char)0x8e; xseg[0x101] = (char)0xc1;	/* mov es,cx */
	chk("hook rc", xstep(), X_OK);
	chk("hook saw paragraph", hookpar & 0xffff, 0xd000);
	chk("hook counted slow", (long)i86nsegslow, 1);
	ntest++;
	if (xm.sb[S_ES] != oseg)
		fail("hook base", 1, 0);
	i86segnew = 0;
}

/* ================================================================== */
/* 3c. paragraph 0, the vector table, and the guest's own handlers     */
/* ================================================================== */

/*
 * The one place in the shim where the GUEST installs the code that runs
 * next.  DDT86 needs all of it: it writes an interrupt vector through a
 * zeroed segment register, enters the program under test with IRET, and
 * expects to be re-entered through a vector when that program hits a
 * breakpoint or steps.
 *
 * Paragraph 0 is a segment in i86spar[]/i86sbase[] like any other, so
 * the guest reads and writes it with ordinary instructions; what makes
 * it the vector table is only that i86exec.c looks there on an INT.  A
 * ZERO vector is not a handler -- it is the shim's spelling of "nobody
 * installed one", and it is what keeps INT 0E0h going to the seam.
 */
static char ivtseg[65536];		/* paragraph 0			*/

/* Paragraph 0 at slot 1 and a second code segment at 0x2000, so a
 * handler can live somewhere the guest did not start. */
static void xsetupivt(void)
{
	xsetup();
	memset(ivtseg, 0, sizeof ivtseg);
	memset(xseg2, 0, sizeof xseg2);
	i86nseg = 3;
	i86spar[1] = 0x0000; i86sbase[1] = ivtseg;
	i86spar[2] = 0x2000; i86sbase[2] = xseg2;
}

/* The four bytes of vector `n', offset first. */
static void setvec(int n, int off, int seg)
{
	ivtseg[n * 4 + 0] = (char)(off & 0xff);
	ivtseg[n * 4 + 1] = (char)((off >> 8) & 0xff);
	ivtseg[n * 4 + 2] = (char)(seg & 0xff);
	ivtseg[n * 4 + 3] = (char)((seg >> 8) & 0xff);
}

/* A word of the guest's stack, `k' words above where SP now points. */
static long stkw(int k)
{
	int sp;

	sp = (xm.r[R_SP] + 2 * k) & 0xffff;
	return ((long)((xseg[sp] & 0xff) | ((xseg[sp + 1] & 0xff) << 8)));
}

static void t_intvec(void)
{
	/* --- the guest writes a vector and reads it back, through a
	   segment register it zeroed itself.  This is DDT86's opening
	   move (its code+0x220 does it with REP MOVSB) and it is what
	   used to stop at X_SEGESC on paragraph 0000. --- */
	xsetupivt();
	put(0x100, "\x2b\xc0", 2);			/* sub ax,ax	*/
	put(0x102, "\x8e\xd8", 2);			/* mov ds,ax	*/
	put(0x104, "\xc7\x06\x0c\x00\x34\x12", 6);	/* mov [0c],1234 */
	put(0x10a, "\x8b\x1e\x0c\x00", 4);		/* mov bx,[0c]	*/
	chk("paragraph 0 sub", xstep(), X_OK);
	chk("paragraph 0 mov ds", xstep(), X_OK);
	chk("paragraph 0 is a segment", xm.sr[S_DS] & 0xffff, 0);
	chk("paragraph 0 unbiased", xm.so[S_DS] & 0xffff, 0);
	chk("vector write", xstep(), X_OK);
	chk("vector low byte", (long)(ivtseg[0x0c] & 0xff), 0x34);
	chk("vector high byte", (long)(ivtseg[0x0d] & 0xff), 0x12);
	chk("vector read back", xstep(), X_OK);
	chk("vector read value", xm.r[R_BX] & 0xffff, 0x1234);

	/* --- INT n through a vector the guest wrote: three words
	   pushed, IF and TF cleared, control at the vector. --- */
	xsetupivt();
	setvec(3, 0x300, 0x2000);
	xm.fl = (i16)(F_ONES | F_IF | F_TF | F_CF);
	xseg[0x100] = (char)0xcc;			/* int 3	*/
	chk("int3 rc", xstep(), X_OK);
	chk("int3 cs", xm.sr[S_CS] & 0xffff, 0x2000);
	chk("int3 ip", xm.ip & 0xffff, 0x300);
	chk("int3 sp", xm.r[R_SP] & 0xffff, 0xfefa);
	chk("int3 pushed ip", stkw(0), 0x101);
	chk("int3 pushed cs", stkw(1), 0x1000);
	chk("int3 pushed flags", stkw(2),
		(long)((F_ONES | F_IF | F_TF | F_CF) & 0xffff));
	chk("int3 cleared if", (long)(xm.fl & F_IF), 0);
	chk("int3 cleared tf", (long)(xm.fl & F_TF), 0);
	chk("int3 kept cf", (long)(xm.fl & F_CF), (long)F_CF);

	/* --- IRET puts back exactly what the entry pushed. --- */
	xseg2[0x300] = (char)0xcf;			/* iret		*/
	chk("iret rc", xstep(), X_OK);
	chk("iret cs", xm.sr[S_CS] & 0xffff, 0x1000);
	chk("iret ip", xm.ip & 0xffff, 0x101);
	chk("iret sp", xm.r[R_SP] & 0xffff, 0xff00);
	chk("iret flags", (long)(xm.fl & 0xffff),
		(long)((F_ONES | F_IF | F_TF | F_CF) & 0xffff));

	/* --- a two-byte INT n reaches the same place, and the address
	   it pushes is past BOTH its bytes. --- */
	xsetupivt();
	setvec(0x20, 0x400, 0x2000);
	put(0x100, "\xcd\x20", 2);			/* int 20h	*/
	chk("int imm rc", xstep(), X_OK);
	chk("int imm ip", xm.ip & 0xffff, 0x400);
	chk("int imm pushed ip", stkw(0), 0x102);

	/* --- nesting: the handler takes an interrupt of its own and
	   unwinds through two IRETs to where the first one began. --- */
	xsetupivt();
	setvec(3, 0x300, 0x2000);
	setvec(1, 0x380, 0x2000);
	xseg[0x100] = (char)0xcc;			/* int 3	*/
	memcpy(&xseg2[0x300], "\xcd\x01", 2);		/* int 1	*/
	xseg2[0x302] = (char)0xcf;			/* iret		*/
	xseg2[0x380] = (char)0xcf;			/* iret		*/
	chk("nest outer", xstep(), X_OK);
	chk("nest inner", xstep(), X_OK);
	chk("nest inner ip", xm.ip & 0xffff, 0x380);
	chk("nest inner sp", xm.r[R_SP] & 0xffff, 0xfef4);
	chk("nest inner pushed ip", stkw(0), 0x302);
	chk("nest inner iret", xstep(), X_OK);
	chk("nest back in handler", xm.ip & 0xffff, 0x302);
	chk("nest outer iret", xstep(), X_OK);
	chk("nest back in guest", xm.ip & 0xffff, 0x101);
	chk("nest cs restored", xm.sr[S_CS] & 0xffff, 0x1000);
	chk("nest sp restored", xm.r[R_SP] & 0xffff, 0xff00);

	/* --- the trap flag, which is what DDT86's T command is.  The
	   instruction runs and the trap follows it, carrying the address
	   AFTER it. --- */
	xsetupivt();
	setvec(1, 0x380, 0x2000);
	xm.fl = (i16)(F_ONES | F_TF);
	xseg[0x100] = (char)0x40;			/* inc ax	*/
	chk("step rc", xstep(), X_OK);
	chk("step ran the instruction", xm.r[R_AX] & 0xffff, 1);
	chk("step trapped", xm.ip & 0xffff, 0x380);
	chk("step pushed ip", stkw(0), 0x101);
	chk("step cleared tf", (long)(xm.fl & F_TF), 0);

	/* --- and it is read at the START of an instruction, so an IRET
	   that SETS it steps what comes after the IRET rather than
	   trapping on the IRET itself.  That is the 8086's rule and it
	   is the one that lets a debugger hand control back. --- */
	xsetupivt();
	setvec(1, 0x380, 0x2000);
	xm.r[R_SP] = 0xfefa;
	xseg[0xfefa] = 0x00; xseg[0xfefb] = 0x02;	/* ip 0200	*/
	xseg[0xfefc] = 0x00; xseg[0xfefd] = 0x10;	/* cs 1000	*/
	xseg[0xfefe] = (char)((F_ONES | F_TF) & 0xff);
	xseg[0xfeff] = (char)(((F_ONES | F_TF) >> 8) & 0xff);
	xseg[0x100] = (char)0xcf;			/* iret		*/
	chk("iret sets tf rc", xstep(), X_OK);
	chk("iret sets tf ip", xm.ip & 0xffff, 0x200);
	chk("iret did not trap", xm.r[R_SP] & 0xffff, 0xff00);
	xseg[0x200] = (char)0x90;			/* nop		*/
	chk("step after iret rc", xstep(), X_OK);
	chk("step after iret trapped", xm.ip & 0xffff, 0x380);
	chk("step after iret pushed ip", stkw(0), 0x201);

	/* --- INT clears TF on the way in, so a stepped INT enters its
	   handler once and is not also stepped. --- */
	xsetupivt();
	setvec(3, 0x300, 0x2000);
	setvec(1, 0x380, 0x2000);
	xm.fl = (i16)(F_ONES | F_TF);
	xseg[0x100] = (char)0xcc;
	chk("stepped int rc", xstep(), X_OK);
	chk("stepped int went to its own handler", xm.ip & 0xffff, 0x300);
	chk("stepped int pushed one frame", xm.r[R_SP] & 0xffff, 0xfefa);

	/* --- WHAT MUST NOT CHANGE.  A zero vector is not a handler, so
	   INT 0E0h is still the seam's, and so is any other interrupt
	   nobody claimed -- IP past the INT, which is the rule
	   i86bdos() is written to. --- */
	xsetupivt();
	put(0x100, "\xcd\xe0", 2);			/* int 0e0h	*/
	chk("seam rc", xstep(), X_INT);
	chk("seam vector", (long)i86intno, 0xe0);
	chk("seam ip past the int", xm.ip & 0xffff, 0x102);
	chk("seam pushed nothing", xm.r[R_SP] & 0xffff, 0xff00);

	/* --- and with NO paragraph 0 at all there is no vector table
	   and nothing is different from before any of this existed. --- */
	xsetup();
	xseg[0x100] = (char)0xcc;			/* int 3	*/
	chk("no ivt rc", xstep(), X_INT);
	chk("no ivt vector", (long)i86intno, 3);
	chk("no ivt ip past the int", xm.ip & 0xffff, 0x101);
	chk("no ivt pushed nothing", xm.r[R_SP] & 0xffff, 0xff00);

	/* --- a trap flag with no vector 1 behind it is inert, which is
	   what it has always been. --- */
	xsetup();
	xm.fl = (i16)(F_ONES | F_TF);
	xseg[0x100] = (char)0x90;			/* nop		*/
	chk("tf with no ivt rc", xstep(), X_OK);
	chk("tf with no ivt did not trap", xm.ip & 0xffff, 0x101);
	chk("tf with no ivt kept tf", (long)(xm.fl & F_TF), (long)F_TF);

	/* --- a vector into a paragraph the shim never handed out is a
	   refusal and not a jump into nothing, and the stack it half
	   wrote is put back. --- */
	xsetupivt();
	setvec(3, 0x300, 0xd000);
	i86nsegbad = 0;
	xseg[0x100] = (char)0xcc;
	chk("bad vector rc", xstep(), X_SEGESC);
	chk("bad vector paragraph", i86segbad & 0xffff, 0xd000);
	chk("bad vector sp restored", xm.r[R_SP] & 0xffff, 0xff00);
	chk("bad vector ip restored", xm.ip & 0xffff, 0x100);

	/* --- the seam answers INT 0E1h as well as 0E0h: DDT86 moves the
	   BDOS up one vector and calls it there. --- */
	xsetupivt();
	xm.r[R_CX] = 12;				/* version	*/
	i86intno = 0xe1;
	chk("int 0e1h serviced", (long)i86bdos(&xm), (long)B_RUN);
	chk("int 0e1h answered", xm.r[R_AX] & 0xffff, (long)(i86ver & 0xffff));
	i86intno = 0xe2;
	chk("int 0e2h still refused", (long)i86bdos(&xm), (long)B_VEC);
}

/* ================================================================== */
/* 4. the loader						      */
/* ================================================================== */

static void mkgrp(char *h, int i, int form, int len, int base, int min, int max)
{
	h[i * 9] = (char)form;
	h[i * 9 + 1] = (char)(len & 0xff);
	h[i * 9 + 2] = (char)((len >> 8) & 0xff);
	h[i * 9 + 3] = (char)(base & 0xff);
	h[i * 9 + 4] = (char)((base >> 8) & 0xff);
	h[i * 9 + 5] = (char)(min & 0xff);
	h[i * 9 + 6] = (char)((min >> 8) & 0xff);
	h[i * 9 + 7] = (char)(max & 0xff);
	h[i * 9 + 8] = (char)((max >> 8) & 0xff);
}

static void t_multi(void);		/* section 4b, below		*/

static void t_loader(void)
{
	char h[CMD_HDR];
	struct i86cmd c;
	struct i86 m;
	char *dseg;

	/* --- an 8080-model header: one code group, G-Max zero --- */
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 882, 0, 888, 0);
	chk("8080 hdr", i86hdr(h, (i32)(128 + 882 * 16), &c), CE_OK);
	chk("8080 model", c.model, M_8080);
	chk("8080 entry", c.entry, 0x100);
	chk("8080 ng", c.ng, 1);
	/* G-Min exceeds G-Length: DDT86's real numbers.  The allocation
	 * is at LEAST G-Min, or the guest's BSS lands outside it -- and
	 * here it is more, because DDT86's G-Max is 0 and a group that
	 * names no maximum gets the segment it owns (galloc()). */
	chk("8080 alloc grows past G-Min", c.g[0].npar, 4096);
	chk("8080 file offset", (long)c.g[0].foff, 128);

	/* --- a small-model header: PIP's real numbers --- */
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 379, 0, 379, 0);
	mkgrp(h, 1, G_DATA, 84, 0, 640, 2176);
	chk("small hdr", i86hdr(h, (i32)7552, &c), CE_OK);
	chk("small model", c.model, M_SMALL);
	chk("small entry", c.entry, 0);
	/* Code: G-Max 0, so the whole segment.  Data: G-Max 2,176, which
	 * is under a segment and is therefore exactly what it gets --
	 * this pair is the one place the two arms of galloc()'s growth
	 * are told apart by real numbers. */
	chk("small code alloc", c.g[0].npar, 4096);
	chk("small data alloc", c.g[1].npar, 2176);
	chk("small data offset", (long)c.g[1].foff, 128 + 379 * 16);
	chk("small need", (long)c.need, 128 + 379 * 16 + 84 * 16);

	/* --- G-Max = 0 is "no maximum", not "no memory" --- */
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 10, 0, 10, 0);
	mkgrp(h, 1, G_DATA, 5, 0, 200, 0);
	chk("gmax0 hdr", i86hdr(h, (i32)(128 + 15 * 16), &c), CE_OK);
	chk("gmax0 alloc", c.g[1].npar, 4096);
	/* and a G-Max that says something is honoured as the ask, not
	 * ignored in favour of G-Min */
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 10, 0, 10, 0);
	mkgrp(h, 1, G_DATA, 5, 0, 200, 300);
	chk("gmax300 hdr", i86hdr(h, (i32)(128 + 15 * 16), &c), CE_OK);
	chk("gmax300 alloc", c.g[1].npar, 300);
	/* G-Max below G-Min does not shrink the allocation */
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 10, 0, 10, 0);
	mkgrp(h, 1, G_DATA, 5, 0, 200, 50);
	chk("gmax under gmin hdr", i86hdr(h, (i32)(128 + 15 * 16), &c),
	    CE_OK);
	chk("gmax under gmin alloc", c.g[1].npar, 200);

	/* --- K1, both halves --- */
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 10, 0x40, 10, 0);
	chk("nonzero A-Base", i86hdr(h, (i32)100000, &c), CE_BASE);
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 10, 0, 10, 0);
	mkgrp(h, 1, G_DATA, 10, 0, 5000, 0);
	chk("over 64K by G-Min", i86hdr(h, (i32)100000, &c), CE_BIG);
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 5000, 0, 5000, 0);
	chk("over 64K by G-Length", i86hdr(h, (i32)100000, &c), CE_BIG);
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 10, 0, 10, 0);
	mkgrp(h, 1, G_DATA, 10, 0, 10, 9000);
	chk("over 64K by G-Max", i86hdr(h, (i32)100000, &c), CE_BIG);
	/* 4,096 paragraphs is exactly one segment and must be allowed:
	 * WordStar declares 4,095 and a 4,096 would be legal too. */
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 100, 0, 4096, 4096);
	chk("exactly 64K allowed", i86hdr(h, (i32)100000, &c), CE_OK);

	/* --- the other refusals --- */
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_DATA, 10, 0, 10, 0);
	chk("no code group", i86hdr(h, (i32)100000, &c), CE_NOCODE);
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 10, 0, 10, 0);
	mkgrp(h, 1, G_SHCODE, 10, 0, 10, 0);
	chk("shared code refused", i86hdr(h, (i32)100000, &c), CE_FORM);
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 10, 0, 10, 0);
	mkgrp(h, 1, G_CODE, 10, 0, 10, 0);
	chk("duplicate form refused", i86hdr(h, (i32)100000, &c), CE_DUP);
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 379, 0, 379, 0);
	mkgrp(h, 1, G_DATA, 84, 0, 640, 2176);
	chk("short file loads", i86hdr(h, (i32)1000, &c), CE_OK);
	chk("short file code bytes", (long)i86have(&c.g[0], (i32)1000),
	    1000L - 128);
	chk("short file data bytes", (long)i86have(&c.g[1], (i32)1000), 0L);
	chk("header-short file refused", i86hdr(h, (i32)127, &c), CE_TRUNC);
	/* trailing padding to the 128-byte record is NOT truncation --
	 * every DRI .CMD has some, PIP has 16 bytes of it */
	chk("padding is not truncation",
	    i86hdr(h, (i32)(128 + 379 * 16 + 84 * 16 + 16), &c), CE_OK);

	/* --- placement and the base page --- */
	dseg = (char *)malloc(65536);
	memset(dseg, 0, 65536);
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 379, 0, 379, 0);
	mkgrp(h, 1, G_DATA, 84, 0, 640, 2176);
	i86hdr(h, (i32)7552, &c);
	memset(&m, 0, sizeof m);
	i86nseg = 2;
	i86spar[0] = 0x1000; i86sbase[0] = xseg;
	i86spar[1] = 0x2000; i86sbase[1] = dseg;
	chk("place small", i86place(&c, &m, 2), CE_OK);
	chk("place cs", m.sr[S_CS] & 0xffff, 0x1000);
	chk("place ds", m.sr[S_DS] & 0xffff, 0x2000);
	chk("place ss follows ds", m.sr[S_SS] & 0xffff, 0x2000);
	chk("place es follows ds", m.sr[S_ES] & 0xffff, 0x2000);
	chk("place ip", m.ip & 0xffff, 0);

	i86bpage(&c, &m, S_DS, "A:VERIFY.OUT=A:VERIFY.IN");
	/* Six bytes to an entry: the 24-bit last byte offset, then the
	 * base paragraph.  The code group filled its segment. */
	chk("bpage code base", (dseg[3] & 0xff) | ((dseg[4] & 0xff) << 8),
	    0x1000);
	chk("bpage code len", (dseg[0] & 0xff) | ((dseg[1] & 0xff) << 8)
	    | ((long)(dseg[2] & 0xff) << 16), 4096L * 16 - 1);
	chk("bpage data base", (dseg[9] & 0xff) | ((dseg[10] & 0xff) << 8),
	    0x2000);
	chk("bpage data len", (dseg[6] & 0xff) | ((dseg[7] & 0xff) << 8)
	    | ((long)(dseg[8] & 0xff) << 16), 2176L * 16 - 1);
	chk("bpage tail len", dseg[0x80] & 0xff, 24);
	ntest++;
	if (memcmp(dseg + 0x81, "A:VERIFY.OUT=A:VERIFY.IN", 24) != 0)
		fail("bpage tail text", 1, 0);
	chk("bpage tail terminated", dseg[0x81 + 24] & 0xff, 0);
	free(dseg);

	/* the 8080 model points every segment register at the one group */
	memset(h, 0, sizeof h);
	mkgrp(h, 0, G_CODE, 882, 0, 888, 0);
	i86hdr(h, (i32)(128 + 882 * 16), &c);
	memset(&m, 0, sizeof m);
	i86nseg = 1;
	i86spar[0] = 0x1000; i86sbase[0] = xseg;
	chk("place 8080", i86place(&c, &m, 1), CE_OK);
	chk("place 8080 cs", m.sr[S_CS] & 0xffff, 0x1000);
	chk("place 8080 ds", m.sr[S_DS] & 0xffff, 0x1000);
	chk("place 8080 ss", m.sr[S_SS] & 0xffff, 0x1000);
	chk("place 8080 es", m.sr[S_ES] & 0xffff, 0x1000);
	/* and IP, which is the whole difference between the two models */
	chk("place 8080 ip", m.ip & 0xffff, 0x100);

	t_multi();
}

/* ================================================================== */
/* 4b. the compact and large models -- a SYNTHESISED multi-group .CMD  */
/* ================================================================== */

/*
 * There is no compact or large .CMD in this tree to read.  All 15 files
 * in the cpm86pc drop are small model or 8080 (tests/i86corpus/SOURCES
 * and the -c sweep), so the only honest way to test the path is to BUILD
 * the header here, from the layout src/cmd/i86load.c documents, and say
 * so -- which is also why nothing binary is shipped for it.
 *
 * What is asserted is the mapping i86place() promises: one segment per
 * declared group, assigned densely in the order code, data, extra,
 * stack, aux 1-4; CS, DS, ES and SS bound to the groups that name them
 * with the small model's fallbacks intact; an auxiliary group given a
 * segment and NO register, reachable only through the base page's group
 * table; and CE_NSEG rather than a silent overlap when the caller has
 * fewer segments than the file has groups.
 *
 * mg() builds one and returns the header length, so every case below is
 * a header and a set of expectations and nothing else.
 */
static long mg(char *h, const int *forms, const int *lens,
	       const int *mins, const int *maxs, int n)
{
	long flen;
	int i;

	memset(h, 0, CMD_HDR);
	flen = CMD_HDR;
	for (i = 0; i < n; i++) {
		mkgrp(h, i, forms[i], lens[i], 0, mins[i], maxs[i]);
		flen += (long)lens[i] * CMD_PARA;
	}
	return (flen);
}

/* One six-byte group-table entry of the base page: the 24-bit last byte
 * offset, then the base paragraph.  `len' is still stated in paragraphs,
 * because that is what the header asks for and what galloc() grants. */
static void chkbp(const char *what, const char *ds, int e, int base, int len)
{
	char w[64];

	sprintf(w, "%s base", what);
	chk(w, (ds[e + 3] & 0xff) | ((ds[e + 4] & 0xff) << 8), base);
	sprintf(w, "%s paragraphs", what);
	chk(w, (ds[e] & 0xff) | ((ds[e + 1] & 0xff) << 8)
	    | ((long)(ds[e + 2] & 0xff) << 16), len ? (long)len * 16 - 1 : 0L);
}

static void t_multi(void)
{
	static char h[CMD_HDR];
	static char seg[CMD_NGRP][65536];
	struct i86cmd c;
	struct i86 m;
	long flen;
	int i;

	static const int lf[5] = { G_CODE, G_DATA, G_EXTRA, G_STACK, G_AUX1 };
	static const int ll[5] = { 2, 2, 1, 1, 1 };
	static const int lm[5] = { 2, 2, 1, 64, 1 };
	static const int lx[5] = { 0, 100, 200, 0, 300 };

	static const int cf[4] = { G_CODE, G_DATA, G_EXTRA, G_STACK };
	static const int cl[4] = { 3, 2, 1, 1 };
	static const int cm[4] = { 3, 2, 1, 1 };
	static const int cx[4] = { 0, 0, 0, 0 };

	static const int sf[2] = { G_CODE, G_STACK };
	static const int sl[2] = { 1, 1 };
	static const int sm[2] = { 1, 32 };
	static const int sx[2] = { 0, 0 };

	static const int af[2] = { G_CODE, G_AUX2 };
	static const int al[2] = { 1, 1 };
	static const int am[2] = { 1, 1 };
	static const int ax[2] = { 0, 64 };

	for (i = 0; i < CMD_NGRP; i++) {
		i86spar[i] = (i16)(0x1000 * (i + 1));
		i86sbase[i] = seg[i];
		memset(seg[i], 0, 256);
	}
	i86nseg = CMD_NGRP;

	/* ---- the large model: five groups, one of them auxiliary ---- */
	flen = mg(h, lf, ll, lm, lx, 5);
	chk("large hdr", i86hdr(h, (i32)flen, &c), CE_OK);
	chk("large model", c.model, M_LARGE);
	chk("large ng", c.ng, 5);
	chk("large entry", c.entry, 0);
	/* galloc() is unchanged and is asserted here anyway, because a
	 * group's paragraph count is what the guest reads out of the base
	 * page: G-Max 0 gets the whole segment, a stated G-Max is the ask,
	 * and G-Min still wins when it is higher. */
	chk("large code alloc", c.g[0].npar, 4096);
	chk("large data alloc", c.g[1].npar, 100);
	chk("large extra alloc", c.g[2].npar, 200);
	chk("large stack alloc", c.g[3].npar, 4096);
	chk("large aux1 alloc", c.g[4].npar, 300);
	chk("large aux1 offset", (long)c.g[4].foff,
	    128 + (2 + 2 + 1 + 1) * 16);

	/* Four segments is one short of five groups, and the answer is a
	 * refusal.  Before this change it was CE_OK with the extra group
	 * still pointing at the data group's segment. */
	memset(&m, 0, sizeof m);
	chk("large refused with four", i86place(&c, &m, 4), CE_NSEG);

	memset(&m, 0, sizeof m);
	chk("large place", i86place(&c, &m, 5), CE_OK);
	chk("large cs", m.sr[S_CS] & 0xffff, 0x1000);
	chk("large ds", m.sr[S_DS] & 0xffff, 0x2000);
	chk("large es is its own group", m.sr[S_ES] & 0xffff, 0x3000);
	chk("large ss is its own group", m.sr[S_SS] & 0xffff, 0x4000);
	chk("large ip", m.ip & 0xffff, 0);
	chk("large warm-boot segment is the stack group",
	    m.wseg & 0xffff, 0x4000);
	/* Four distinct host segments in the four registers: the whole
	 * point of the compact and large models, and the thing the old
	 * code could not do. */
	ntest++;
	if (m.sb[S_CS] == m.sb[S_DS] || m.sb[S_DS] == m.sb[S_ES]
	 || m.sb[S_ES] == m.sb[S_SS] || m.sb[S_CS] == m.sb[S_SS])
		fail("large four distinct host segments", 1, 0);
	chk("large code slot", c.g[0].sidx, 0);
	chk("large data slot", c.g[1].sidx, 1);
	chk("large extra slot", c.g[2].sidx, 2);
	chk("large stack slot", c.g[3].sidx, 3);
	/* The auxiliary group: a segment of its own, and no register. */
	chk("large aux1 slot", c.g[4].sidx, 4);
	chk("large aux1 paragraph", c.g[4].par & 0xffff, 0x5000);
	chk("large aux1 has no register", c.g[4].seg, S_NONE);
	ntest++;
	if (i86sbase[c.g[4].sidx] != seg[4])
		fail("large aux1 host segment", 1, 0);
	/* ... so the base page is the only way the guest can learn it,
	 * which is what the group table at 0x00-0x2F is for. */
	i86bpage(&c, &m, S_DS, "");
	chkbp("large bpage code", seg[1], 0x00, 0x1000, 4096);
	chkbp("large bpage data", seg[1], 0x06, 0x2000, 100);
	chkbp("large bpage extra", seg[1], 0x0c, 0x3000, 200);
	chkbp("large bpage stack", seg[1], 0x12, 0x4000, 4096);
	chkbp("large bpage aux1", seg[1], 0x18, 0x5000, 300);
	/* and the three entries no group filled are still zero */
	chkbp("large bpage aux2", seg[1], 0x1e, 0, 0);
	chkbp("large bpage aux4", seg[1], 0x2a, 0, 0);
	/* A paragraph in the table resolves even though no register
	 * holds it -- this is what the guest's `mov es,[0x10]' does. */
	ntest++;
	if (i86resolve((i16)0x5000) != seg[4])
		fail("large aux1 resolves", 1, 0);

	/* ---- the compact model: four groups, no auxiliary ---- */
	flen = mg(h, cf, cl, cm, cx, 4);
	chk("compact hdr", i86hdr(h, (i32)flen, &c), CE_OK);
	chk("compact model", c.model, M_COMPACT);
	chk("compact ng", c.ng, 4);
	memset(&m, 0, sizeof m);
	chk("compact refused with three", i86place(&c, &m, 3), CE_NSEG);
	memset(&m, 0, sizeof m);
	chk("compact place", i86place(&c, &m, 4), CE_OK);
	chk("compact cs", m.sr[S_CS] & 0xffff, 0x1000);
	chk("compact ds", m.sr[S_DS] & 0xffff, 0x2000);
	chk("compact es", m.sr[S_ES] & 0xffff, 0x3000);
	chk("compact ss", m.sr[S_SS] & 0xffff, 0x4000);

	/* ---- density: a stack group with no extra group takes slot 1 ----
	 * Not slot 3.  A mapping keyed on the form rather than on what the
	 * file declares would ask this machine for four segments to place
	 * two groups, and this machine has seven. */
	flen = mg(h, sf, sl, sm, sx, 2);
	chk("code+stack hdr", i86hdr(h, (i32)flen, &c), CE_OK);
	chk("code+stack model", c.model, M_COMPACT);
	memset(&m, 0, sizeof m);
	chk("code+stack place with two", i86place(&c, &m, 2), CE_OK);
	chk("code+stack stack slot", c.g[1].sidx, 1);
	chk("code+stack ss", m.sr[S_SS] & 0xffff, 0x2000);
	/* No data group, so DS falls back to the code group and the base
	 * page still has somewhere to live. */
	chk("code+stack ds falls back to code", m.sr[S_DS] & 0xffff, 0x1000);
	chk("code+stack es follows ds", m.sr[S_ES] & 0xffff, 0x1000);

	/* ---- an auxiliary group with no extra and no stack ---- */
	flen = mg(h, af, al, am, ax, 2);
	chk("code+aux2 hdr", i86hdr(h, (i32)flen, &c), CE_OK);
	chk("code+aux2 model", c.model, M_LARGE);
	memset(&m, 0, sizeof m);
	chk("code+aux2 place with two", i86place(&c, &m, 2), CE_OK);
	chk("code+aux2 slot", c.g[1].sidx, 1);
	chk("code+aux2 paragraph", c.g[1].par & 0xffff, 0x2000);
	chk("code+aux2 has no register", c.g[1].seg, S_NONE);
	chk("code+aux2 ds is the code group", m.sr[S_DS] & 0xffff, 0x1000);
	/* aux2's table entry is the SECOND of the four, at 0x1e */
	memset(seg[0], 0, 256);
	i86bpage(&c, &m, S_CS, "");
	chkbp("code+aux2 bpage code", seg[0], 0x00, 0x1000, 4096);
	chkbp("code+aux2 bpage aux2", seg[0], 0x1e, 0x2000, 64);

	/* ---- eight groups: the format's ceiling, which the HOST can
	 * place and this machine cannot.  Seven logical segments exist
	 * (src/bios/pgalloc.c) and src/cmd/i86.c spends one of them
	 * staging the file, so the target's ceiling is six; the loader's
	 * is whatever it is handed.  Asserted here so that the two
	 * numbers are written down in a place that fails when they
	 * change. ---- */
	{
		static const int ef[8] = { G_CODE, G_DATA, G_EXTRA, G_STACK,
					   G_AUX1, G_AUX2, G_AUX3, G_AUX4 };
		static const int el[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
		static const int em[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
		static const int ex[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

		flen = mg(h, ef, el, em, ex, 8);
		chk("eight hdr", i86hdr(h, (i32)flen, &c), CE_OK);
		chk("eight ng", c.ng, 8);
		chk("eight model", c.model, M_LARGE);
		memset(&m, 0, sizeof m);
		chk("eight refused with six", i86place(&c, &m, 6), CE_NSEG);
		memset(&m, 0, sizeof m);
		chk("eight refused with seven", i86place(&c, &m, 7), CE_NSEG);
		memset(&m, 0, sizeof m);
		chk("eight placed with eight", i86place(&c, &m, 8), CE_OK);
		for (i = 0; i < 8; i++) {
			char w[64];
			sprintf(w, "eight slot %d", i);
			chk(w, c.g[i].sidx, i);
		}
		chk("eight aux4 paragraph", c.g[7].par & 0xffff, 0x8000);
	}
	chk("CE_NSEG has a sentence", (long)(i86cerr(CE_NSEG)[0] != 0), 1);
}

/* ================================================================== */
/* 5. the corpus sweep (by hand; no make target may reach it)	      */
/* ================================================================== */

static const char *gform(int f)
{
	static const char *n[] = { "-", "code", "data", "extra", "stack",
				   "aux1", "aux2", "aux3", "aux4", "shcode" };
	return (f >= 0 && f <= 9 ? n[f] : "?");
}

/* The classes i86exec.c decodes and deliberately refuses.  Kept here
 * rather than exported, so that the sweep's "what stage one cannot run
 * yet" number is a statement made by the TEST about the executor and
 * not one the executor makes about itself. */
static int unimp(int op)
{
	return (op == I_STRING || op == I_CALLF || op == I_RETF
	     || op == I_JMPF || op == I_IRET || op == I_INTO
	     || op == I_ESC || op == I_IO || op == I_WAIT
	     || op == I_NIMPL);
}

#define NMN 80

static int sweep(const char *path)
{
	static char seg[65536];
	static const char *mnname[NMN];
	static long mncount[NMN];
	struct i86cmd c;
	struct i86in in;
	FILE *fp;
	long flen;
	char h[CMD_HDR];
	int rc, i, n, nmn;
	i16 ip;
	long ninsn, badn, nunimp;

	fp = fopen(path, "rb");
	if (!fp) {
		printf("%s: cannot open\n", path);
		return (1);
	}
	fseek(fp, 0L, SEEK_END);
	flen = ftell(fp);
	fseek(fp, 0L, SEEK_SET);
	if (fread(h, 1, CMD_HDR, fp) != CMD_HDR) {
		printf("%s: short header\n", path);
		fclose(fp);
		return (1);
	}
	rc = i86hdr(h, (i32)flen, &c);
	printf("%-28s %6ld bytes  %s  %s\n", path, flen,
		rc == CE_OK ? (c.model == M_8080 ? "8080 " : "small")
			    : "     ",
		i86cerr(rc));
	for (i = 0; i < CMD_NGRP; i++)
		if (c.g[i].form)
			printf("      [%d] %-6s len=%5u base=%5u min=%5u "
				"max=%5u -> alloc %5u par at file+%ld\n",
				i, gform(c.g[i].form), c.g[i].len,
				c.g[i].base, c.g[i].min, c.g[i].max,
				c.g[i].npar, (long)c.g[i].foff);
	if (rc != CE_OK) {
		fclose(fp);
		return (1);
	}
	if (flen > (long)c.need)
		printf("      %ld bytes of trailing padding (to a 128-byte "
			"record)\n", flen - (long)c.need);

	memset(seg, 0, sizeof seg);
	fseek(fp, (long)c.g[0].foff, SEEK_SET);
	n = (int)fread(seg, 1, (size_t)c.g[0].len * CMD_PARA, fp);
	fclose(fp);
	for (i = 0; i < NMN; i++)
		mncount[i] = 0;
	nmn = 0;
	badn = 0;
	ninsn = 0;
	nunimp = 0;
	ip = (i16)(c.model == M_8080 ? 0x100 : 0);
	while ((long)(unsigned)ip < (long)n) {
		i86dec(seg, ip, &in);
		if (in.op == I_BAD)
			badn++;
		else {
			const char *mn = i86mnem(&in);
			for (i = 0; i < nmn; i++)
				if (strcmp(mnname[i], mn) == 0)
					break;
			if (i == nmn && nmn < NMN)
				mnname[nmn++] = mn;
			if (i < NMN)
				mncount[i]++;
			if (unimp(in.op))
				nunimp++;
		}
		ninsn++;
		ip = (i16)(ip + in.len);
	}
	printf("      %ld instructions, %d distinct mnemonics, %ld "
		"undecodable bytes\n", ninsn, nmn, badn);
	printf("      %ld instructions (%ld%%) are in a class stage one "
		"does not execute\n", nunimp,
		ninsn ? nunimp * 100 / ninsn : 0L);
	printf("     ");
	for (i = 0; i < nmn; i++)
		printf(" %s=%ld", mnname[i], mncount[i]);
	printf("\n");
	return (0);
}

/* ================================================================== */
/* 6. loading real files: the corpus in tests/i86corpus/		      */
/* ================================================================== */

/*
 * tests/i86corpus/ holds four of Digital Research's own CP/M-86 programs
 * -- PIP, ED, GENCMD and SUBMIT, taken from the cpm86pc drop of CP/M-86
 * 1.0 and unmodified.  They are covered by the 9 July 2022 DRDOS, Inc.
 * grant (Bryan Sparks; cpm.z80.de/license.html), which carries no
 * distribution restriction.
 *
 * They are TEST INPUT, and nothing else.  Nothing here is staged onto a
 * disk image, and none of it is a program this system means to run: our
 * utilities are native Z8000 code -- src/cmd/pip.c and src/cmd/stat.c are
 * DRI's own sources rebuilt as .Z8K, and they will beat anything the shim
 * interprets, forever (CPM86-STAGE-ONE.md §2.4).  PIP.CMD earns its place
 * for one reason: a file copy is a known-answer test, so when the INT 0E0h
 * seam exists, `cmp' can decide whether the shim worked.
 *
 * What a real file can test is that the loader reads what a real header
 * says.  What it cannot test is every refusal: all four are small-model,
 * all carry A-Base 0, none is malformed.  build/cmdfix/ is that half,
 * generated by tools/mkcmdfix.py at test time.  Both go through the SAME
 * three calls below, so the path that reads PIP is the path that has to
 * refuse a 40-byte file.
 */

struct ld {
	struct i86cmd c;
	struct i86 m;
	long	flen;
	char	*img;
};

/*
 * Eight, not two: a .CMD header can describe eight groups and the
 * compact and large models actually use them (src/cmd/i86load.c
 * i86place()).  The corpus needs two and gets two -- ldplace() below
 * hands i86place() exactly as many segments as the file has groups, so
 * a small-model file is placed against the same i86nseg = 2 and the same
 * paragraphs 0x1000/0x2000 it always was, and nothing about the corpus
 * runs moves.  THE MACHINE cannot supply eight; see i86place()'s comment
 * and docs/cpm/docs/run/E1.md.  The host can, so the host is where the
 * general case is tested.
 */
static char lseg[CMD_NGRP][65536];

/* Paragraph 0: the low 64 KB, holding the interrupt vector table.  It is
 * registered after the groups, so a paragraph inside a group's own
 * segment still resolves to that group.  The target gate keeps the
 * segment it staged the file in for this (src/cmd/i86.c). */
static char zseg[65536];

/*
 * The spare end of lseg[]: whatever a placed program did not need is
 * what BDOS function 59 can give a program the guest loads.  The
 * machine's pool is seven segments and this one is eight, so the host
 * is the wider of the two and the seam's own ceiling -- I86NSEG slots
 * in i86spar[] -- is what either of them runs into first.
 */
static int lstaken[CMD_NGRP];

static char *hsegget(void)
{
	int i;

	for (i = 0; i < CMD_NGRP; i++)
		if (!lstaken[i]) {
			lstaken[i] = 1;
			return (lseg[i]);
		}
	return ((char *)0);
}

static int hsegput(char *b)
{
	int i;

	for (i = 0; i < CMD_NGRP; i++)
		if (lseg[i] == b) {
			lstaken[i] = 0;
			return (1);
		}
	return (0);
}

static int ldread(const char *path, struct ld *L)
{
	FILE *fp;
	char h[CMD_HDR];
	long n;

	memset(L, 0, sizeof *L);
	fp = fopen(path, "rb");
	if (fp == 0)
		return (-1);
	fseek(fp, 0L, SEEK_END);
	L->flen = ftell(fp);
	fseek(fp, 0L, SEEK_SET);
	L->img = (char *)malloc((size_t)L->flen + 1);
	n = (long)fread(L->img, 1, (size_t)L->flen, fp);
	fclose(fp);
	if (n != L->flen)
		return (-1);
	/* A short file is read into a ZEROED header buffer and its real
	 * length handed to i86hdr(), which is the only honest way to ask
	 * "is this a header at all": the bytes that are not there must not
	 * be whatever the buffer held last. */
	memset(h, 0, sizeof h);
	memcpy(h, L->img, (size_t)(L->flen < CMD_HDR ? L->flen : CMD_HDR));
	return (i86hdr(h, (i32)L->flen, &L->c));
}

static void ldfree(struct ld *L)
{
	if (L->img)
		free(L->img);
	L->img = 0;
}

/*
 * Place the groups, copy their images in, build the base page, and put
 * the stack at the top of the allocation -- the whole of what a loader
 * does between "the header is good" and "start the guest".  One 64 KB
 * host segment per declared group -- i86place()'s own rule -- so a
 * small-model file gets the two it always got and a large-model one gets
 * as many as it declares.
 *
 * The images are copied through g->sidx and NOT through g->seg, because
 * an auxiliary group has a segment and no segment register: g->seg is
 * S_NONE for it and i86sbase[g->sidx] is the only handle there is.  The
 * target gate (src/cmd/i86.c) copies them the same way for the same
 * reason.
 */
static int ldplace(struct ld *L, const char *tail)
{
	register struct i86grp *g;
	int i, rc, slot, n;
	long np;

	n = L->c.ng < 1 ? 1 : L->c.ng;
	if (n > CMD_NGRP)
		n = CMD_NGRP;
	memset(lstaken, 0, sizeof lstaken);
	i86segget = hsegget;
	i86segput = hsegput;
	for (i = 0; i < n; i++) {
		lstaken[i] = 1;
		memset(lseg[i], 0, sizeof lseg[i]);
		i86spar[i] = (i16)(0x1000 * (i + 1));
		i86sbase[i] = lseg[i];
	}
	memset(zseg, 0, sizeof zseg);
	i86spar[n] = (i16)0;
	i86sbase[n] = zseg;
	i86nseg = n + 1;
	L->m.fl = F_ONES;
	L->m.lz = LZ_NONE;
	rc = i86place(&L->c, &L->m, n);
	if (rc != CE_OK)
		return (rc);
	for (i = 0; i < CMD_NGRP; i++) {
		g = &L->c.g[i];
		if (g->form == G_NONE || g->form > G_AUX4)
			continue;
		memcpy(i86sbase[g->sidx], L->img + g->foff,
			(size_t)i86have(g, (i32)L->flen));
	}
	/* The base page goes at the base of the group DS names -- the data
	 * group in the small model, the one group in the 8080 model, where
	 * it lands on the 256 zero bytes the image supplies for it. */
	slot = L->c.model == M_8080 ? S_CS : S_DS;
	i86bpage(&L->c, &L->m, slot, (char *)tail);
	np = 0;
	for (i = 0; i < CMD_NGRP; i++)
		if (L->c.g[i].form && L->c.g[i].seg == slot)
			np = L->c.g[i].npar;
	if (np == 0)
		np = L->c.g[0].npar;
	L->m.r[R_SP] = (i16)(np >= CMD_MAXPAR ? 0xfffe : np * CMD_PARA);
	return (CE_OK);
}

static int ldrun(struct ld *L, long maxstep, long *nstep)
{
	struct i86in in;
	int rc;
	long k;

	for (k = 0; k < maxstep; k++) {
		rc = i86step(&L->m, &in);
		if (rc != X_OK) {
			*nstep = k + 1;
			return (rc);
		}
	}
	*nstep = k;
	return (X_OK);
}

/* Decode the code group end to end and count what comes out: instructions,
 * bytes that are not an instruction, and instructions in a class stage one
 * refuses.  Same walk as the -c sweep, but the numbers are asserted. */
static void ldsweep(struct ld *L, long *ninsn, long *nbad, long *nunimp)
{
	struct i86in in;
	long n;
	i16 ip;

	*ninsn = *nbad = *nunimp = 0;
	n = (long)L->c.g[0].len * CMD_PARA;
	ip = (i16)(L->c.model == M_8080 ? 0x100 : 0);
	while ((long)(unsigned)ip < n) {
		i86dec(L->m.sb[S_CS], ip, &in);
		if (in.op == I_BAD)
			(*nbad)++;
		else if (unimp(in.op))
			(*nunimp)++;
		(*ninsn)++;
		ip = (i16)(ip + in.len);
	}
}

/*
 * The small-model files, and every number here was measured with the -c
 * sweep and then written down -- none of it is a guess about the format.
 *
 * All are small-model with G-Max 0 in the code group, which is the
 * shape i86load.c's two findings describe: G-Min above G-Length in every
 * one of them (PIP supplies 84 paragraphs of data and asks for 640), and
 * G-Max 0 in every code group, meaning "no maximum" and not "no memory".
 * No file is shorter than its groups; most are longer, because a .CMD is
 * padded to a 128-byte CP/M record, and that padding is not truncation.
 *
 * The two `npar' columns are the only ones here that are not read out of
 * the file: they are what galloc() grants, and every one of them moved on
 * 2026-09-04 when galloc() started growing a group toward G-Max.  Every
 * code group has G-Max 0 and so gets the whole segment (4,096); the data
 * groups get their G-Max where they state one -- PIP 2,176, ED 4,095,
 * GENCMD 4,080 -- and the segment where they do not, which is SUBMIT.
 *
 * The instruction counts are the decoder's, not objdump's -- the
 * cross-check against `objdump -m i8086' over these code groups is in the
 * merged commit's message and needs objdump, so it cannot live here.  What
 * lives here is the property that matters for a linear sweep: not one byte
 * of DRI's code fails to decode.  The `unimp' column is the measurement
 * CPM86-STAGE-ONE.md 2.3 scoped stage one on: PIP and ED contain zero
 * instructions stage one refuses, GENCMD and SUBMIT one each, and both are
 * an IN.
 */
struct crow {
	const char *name;
	long	flen, need, ninsn;
	int	nunimp;
	int	clen, cmin, cmax, cnpar;
	int	dlen, dmin, dmax, dnpar;
	long	pstep;		/* instructions from entry to the first INT */
	int	pip;		/* and where it is			*/
};

static struct crow crows[] = {
	{"PIP.CMD",    7552, 7536, 2487, 0, 379, 379,    0, 4096,
					  84, 640, 2176, 2176, 1581, 0x001b},
	{"ED.CMD",     9472, 9360, 3119, 0, 485, 485,    0, 4096,
					  92, 256, 4095, 4095,   25, 0x006b},
	{"GENCMD.CMD", 5760, 5648, 1357, 1, 211, 211,    0, 4096,
					 134, 688, 4080, 4080,  515, 0x0029},
	{"SUBMIT.CMD", 3968, 3936,  435, 1,  65,  65,    0, 4096,
					 173, 173,    0, 4096, 1625, 0x006b},
	{"ASM86.CMD", 26240, 26240, 8015, 32, 1197, 1197, 0, 4096,
					 435, 1102, 4095, 4095,  362, 0x0086},
	{"STAT.CMD",   9344, 9344, 2400,  3, 360, 360,    0, 4096,
					 216, 848, 2048, 2048,   24, 0x0021},
	{"HELP.CMD",   6656, 6560, 1865,  1, 294, 294,    0, 4096,
					 108, 364, 4095, 4095, 1121, 0x001b},
	{0}
};

static char nmbuf[128];

static const char *nm(const char *a, const char *b)
{
	sprintf(nmbuf, "%s %s", a, b);
	return (nmbuf);
}

static void t_corpus(const char *dir)
{
	struct crow *r;
	struct ld L;
	char path[512];
	long ninsn, nbad, nunimp, nstep;
	int rc;

	for (r = crows; r->name; r++) {
		sprintf(path, "%s/%s", dir, r->name);
		rc = ldread(path, &L);
		if (rc < 0) {
			fail(nm(r->name, "unreadable"), 0, 0);
			continue;
		}
		chk(nm(r->name, "rc"), rc, CE_OK);
		chk(nm(r->name, "file length"), L.flen, r->flen);
		if (rc != CE_OK) {
			ldfree(&L);
			continue;
		}
		chk(nm(r->name, "model"), L.c.model, M_SMALL);
		chk(nm(r->name, "entry"), L.c.entry, 0);
		chk(nm(r->name, "ng"), L.c.ng, 2);
		chk(nm(r->name, "need"), (long)L.c.need, r->need);
		ntest++;
		if (L.flen < (long)L.c.need)
			fail(nm(r->name, "record padding"), L.flen, r->need);
		chk(nm(r->name, "code form"), L.c.g[0].form, G_CODE);
		chk(nm(r->name, "code A-Base"), L.c.g[0].base, 0);
		chk(nm(r->name, "code G-Length"), L.c.g[0].len, r->clen);
		chk(nm(r->name, "code G-Min"), L.c.g[0].min, r->cmin);
		chk(nm(r->name, "code G-Max"), L.c.g[0].max, r->cmax);
		chk(nm(r->name, "code alloc"), L.c.g[0].npar, r->cnpar);
		chk(nm(r->name, "code at file+128"), (long)L.c.g[0].foff, 128);
		chk(nm(r->name, "data form"), L.c.g[1].form, G_DATA);
		chk(nm(r->name, "data A-Base"), L.c.g[1].base, 0);
		chk(nm(r->name, "data G-Length"), L.c.g[1].len, r->dlen);
		chk(nm(r->name, "data G-Min"), L.c.g[1].min, r->dmin);
		chk(nm(r->name, "data G-Max"), L.c.g[1].max, r->dmax);
		chk(nm(r->name, "data alloc"), L.c.g[1].npar, r->dnpar);
		chk(nm(r->name, "data offset"), (long)L.c.g[1].foff,
			128 + (long)r->clen * 16);
		/* G-Min above G-Length is DRI's own shape, in all of them */
		ntest++;
		if (L.c.g[1].min < L.c.g[1].len)
			fail(nm(r->name, "data G-Min >= G-Length"), 0, 0);

		chk(nm(r->name, "place"), ldplace(&L, FIXTAIL), CE_OK);
		chk(nm(r->name, "cs"), L.m.sr[S_CS] & 0xffff, 0x1000);
		chk(nm(r->name, "ds"), L.m.sr[S_DS] & 0xffff, 0x2000);
		chk(nm(r->name, "ip"), L.m.ip & 0xffff, 0);
		/* the base page the guest will read, over a real header */
		chk(nm(r->name, "bpage data paragraphs"),
			(L.m.sb[S_DS][6] & 0xff)
			| ((L.m.sb[S_DS][7] & 0xff) << 8)
			| ((long)(L.m.sb[S_DS][8] & 0xff) << 16),
			(long)r->dnpar * 16 - 1);
		ldsweep(&L, &ninsn, &nbad, &nunimp);
		chk(nm(r->name, "instructions"), ninsn, r->ninsn);
		chk(nm(r->name, "undecodable bytes"), nbad, 0);
		chk(nm(r->name, "refused classes"), nunimp, r->nunimp);
		/*
		 * And then RUN it, from the entry point the loader chose,
		 * until it asks the operating system for something.
		 *
		 * All of them reach INT 0E0h -- the CP/M-86 BDOS entry -- with
		 * no refusal and no undecodable byte on the way, which is
		 * the strongest statement this file can make before the
		 * seam behind that INT exists: the image was placed where
		 * the program expects it, the base page it reads is the one
		 * we built, and its whole prologue executes.  ED gets there
		 * in 25 instructions and SUBMIT in 1,625.
		 *
		 * PIP's count moves with the command tail, because it
		 * scans it: an empty tail costs one instruction fewer.
		 * FIXTAIL is what makes this number reproducible.
		 */
		rc = (int)ldrun(&L, 100000L, &nstep);
		chk(nm(r->name, "prologue rc"), rc, X_INT);
		chk(nm(r->name, "prologue vector"), i86intno, 0xe0);
		chk(nm(r->name, "prologue steps"), nstep, r->pstep);
		chk(nm(r->name, "prologue ip"), L.m.ip & 0xffff, r->pip);
		ldfree(&L);
	}
}

/* ================================================================== */
/* 7. the generated fixtures (tools/mkcmdfix.py)			      */
/* ================================================================== */

struct nmap {
	const char *n;
	int	v;
};

static struct nmap cemap[] = {
	{"CE_OK", CE_OK}, {"CE_NOCODE", CE_NOCODE}, {"CE_BASE", CE_BASE},
	{"CE_BIG", CE_BIG}, {"CE_FORM", CE_FORM}, {"CE_TRUNC", CE_TRUNC},
	{"CE_EMPTY", CE_EMPTY}, {"CE_DUP", CE_DUP}, {0, 0}
};

static struct nmap xmap[] = {
	{"X_OK", X_OK}, {"X_UNIMP", X_UNIMP}, {"X_BAD", X_BAD},
	{"X_INT", X_INT}, {"X_HALT", X_HALT}, {"X_SEGESC", X_SEGESC},
	{"X_WINDOW", X_WINDOW}, {"X_WBOOT", X_WBOOT}, {0, 0}
};

static struct nmap segmap[] = {
	{"es", S_ES}, {"cs", S_CS}, {"ss", S_SS}, {"ds", S_DS}, {0, 0}
};

static struct nmap regmap[] = {
	{"ax", R_AX}, {"cx", R_CX}, {"dx", R_DX}, {"bx", R_BX},
	{"sp", R_SP}, {"bp", R_BP}, {"si", R_SI}, {"di", R_DI}, {0, 0}
};

static int lookup(struct nmap *t, const char *s)
{
	for (; t->n; t++)
		if (strcmp(t->n, s) == 0)
			return (t->v);
	return (-1);
}

/* "key=value" -> value, or 0 with *got clear. */
static long kval(const char *tok, const char *key, int *got)
{
	int n;

	n = (int)strlen(key);
	*got = 0;
	if (strncmp(tok, key, (size_t)n) != 0 || tok[n] != '=')
		return (0);
	*got = 1;
	return (strtol(tok + n + 1, (char **)0, 0));
}

static int nfix;

static void fixline(const char *dir, char *line)
{
	char *tok[32];
	char path[512];
	struct ld L;
	const char *name;
	char *p, *q;
	long v, nstep;
	int nt, i, want, rc, got, sg;

	nt = 0;
	for (p = strtok(line, " \t\r\n"); p && nt < 32;
	     p = strtok((char *)0, " \t\r\n")) {
		if (*p == '#')
			break;
		tok[nt++] = p;
	}
	if (nt == 0 || tok[0][0] == '#')
		return;
	if (nt < 3) {
		fail("fixture manifest line", nt, 3);
		return;
	}
	name = tok[1];
	sprintf(path, "%s/%s", dir, name);
	nfix++;

	if (strcmp(tok[0], "LOAD") == 0) {
		want = lookup(cemap, tok[2]);
		if (want < 0) {
			fail(nm(name, "unknown CE_* name"), 0, 0);
			return;
		}
		rc = ldread(path, &L);
		if (rc < 0) {
			fail(nm(name, "unreadable"), 0, 0);
			return;
		}
		chk(nm(name, "load rc"), rc, want);
		/* the refusal has to be able to say why, in words */
		ntest++;
		if (i86cerr(rc) == 0 || *i86cerr(rc) == 0)
			fail(nm(name, "refusal text"), 0, 0);
		for (i = 3; i < nt; i++) {
			if (strncmp(tok[i], "model=", 6) == 0) {
				const char *mn = tok[i] + 6;
				int mv = strcmp(mn, "8080") == 0 ? M_8080
				       : strcmp(mn, "small") == 0 ? M_SMALL
				       : strcmp(mn, "compact") == 0 ? M_COMPACT
				       : strcmp(mn, "large") == 0 ? M_LARGE
				       : -1;
				if (mv < 0)
					fail(nm(name, "unknown model name"),
						0, 0);
				else
					chk(nm(name, "model"), L.c.model, mv);
			}
			v = kval(tok[i], "entry", &got);
			if (got)
				chk(nm(name, "entry"), L.c.entry, v);
			v = kval(tok[i], "ng", &got);
			if (got)
				chk(nm(name, "ng"), L.c.ng, v);
			v = kval(tok[i], "need", &got);
			if (got)
				chk(nm(name, "need"), (long)L.c.need, v);
			if (strncmp(tok[i], "alloc=", 6) == 0) {
				q = tok[i] + 6;
				for (sg = 0; sg < CMD_NGRP && *q; sg++) {
					while (sg < CMD_NGRP
					     && L.c.g[sg].form == G_NONE)
						sg++;
					v = strtol(q, &q, 0);
					chk(nm(name, "alloc"),
						L.c.g[sg].npar, v);
					if (*q == ',')
						q++;
				}
			}
		}
		ldfree(&L);
		return;
	}
	if (strcmp(tok[0], "RUN") != 0) {
		fail("fixture manifest verb", 0, 0);
		return;
	}

	want = lookup(xmap, tok[2]);
	if (want < 0) {
		fail(nm(name, "unknown X_* name"), 0, 0);
		return;
	}
	rc = ldread(path, &L);
	if (rc < 0) {
		fail(nm(name, "unreadable"), 0, 0);
		return;
	}
	chk(nm(name, "run load"), rc, CE_OK);
	if (rc != CE_OK) {
		ldfree(&L);
		return;
	}
	chk(nm(name, "run place"), ldplace(&L, FIXTAIL), CE_OK);
	nstep = 0;
	rc = ldrun(&L, 1000L, &nstep);
	chk(nm(name, "run rc"), rc, want);
	for (i = 3; i < nt; i++) {
		v = kval(tok[i], "ip", &got);
		if (got)
			chk(nm(name, "ip"), L.m.ip & 0xffff, v);
		v = kval(tok[i], "steps", &got);
		if (got)
			chk(nm(name, "steps"), nstep, v);
		for (sg = 0; regmap[sg].n; sg++) {
			v = kval(tok[i], regmap[sg].n, &got);
			if (got)
				chk(nm(name, regmap[sg].n),
					L.m.r[regmap[sg].v] & 0xffff, v);
		}
		if (strncmp(tok[i], "w=", 2) == 0) {
			q = tok[i] + 2;
			p = strchr(q, ':');
			if (p == 0) {
				fail(nm(name, "w= syntax"), 0, 0);
				continue;
			}
			*p = 0;
			sg = lookup(segmap, q);
			q = p + 1;
			v = strtol(q, &q, 0);	/* offset */
			if (sg < 0 || *q != ':')
				fail(nm(name, "w= syntax"), 0, 0);
			else
				chk(nm(name, "memory word"),
					(L.m.sb[sg][v] & 0xff)
					| ((L.m.sb[sg][v + 1] & 0xff) << 8),
					strtol(q + 1, (char **)0, 0));
		}
	}
	ldfree(&L);
}

static void t_fixtures(const char *dir)
{
	char path[512];
	char line[512];
	FILE *fp;

	sprintf(path, "%s/MANIFEST", dir);
	fp = fopen(path, "r");
	if (fp == 0) {
		fail("fixture manifest missing (run tools/mkcmdfix.py)", 0, 0);
		return;
	}
	nfix = 0;
	while (fgets(line, sizeof line, fp))
		fixline(dir, line);
	fclose(fp);
	/* An empty fixture set must not report success: the generator can
	 * fail and leave a directory behind (tests/relcheck.sh's rule). */
	ntest++;
	if (nfix < 15)
		fail("fixtures found", nfix, 15);
}

/* ================================================================== */
/* 8. the INT 0E0h seam (src/cmd/i86bdos.c)			      */
/* ================================================================== */

/*
 * i86bdos.c reaches the native BDOS through exactly one function,
 * i86sys(), and that is the seam's own seam: on the target it is one
 * line around __bdos(), and here it is whatever this file wants it to
 * be.  Two things want different ones.
 *
 *   8a asserts the MAPPING.  A recording i86sys() writes down the
 *	function, the value and the address it was handed, so every
 *	claim about the calling convention -- DL for a byte, DS:DX for
 *	an FCB, 36 bytes of it, function 12 answered without a call at
 *	all, function 26 and 51 recomputing one native address between
 *	them -- is a check with a number rather than a paragraph.
 *
 *   8b RUNS PIP.  A stub CP/M behind the same i86sys() gives the guest
 *	a small in-memory disk, and the gate's own command line is
 *	handed to DRI's PIP.CMD: copy a file, then compare the copy with
 *	the original, byte for byte.  It is CPM86-STAGE-ONE.md §5.3
 *	step 1 -- "run PIP against it on the host, with the BDOS calls
 *	stubbed to the host filesystem" -- and it is the cheapest thing
 *	in the plan that can still return "no", because it converts the
 *	static 40-mnemonic count into a dynamic one and because a file
 *	copy is a known-answer test.
 *
 * It is NOT the gate.  Gate M1'a is this same command on the emulator
 * through the real BDOS, and nothing here can stand in for it: the
 * whole point of §5.1 is that host C passing is not a target verdict.
 */


/* ---- 7c: the two default FCBs, which the CCP fills in ---- */

/*
 * i86bpage() builds the base page, and until a SECOND real binary ran
 * it left 0x5C and 0x6C as the 256 zero bytes it had cleared.  PIP
 * parses its own command tail, so nothing noticed.  SUBMIT.CMD and
 * GENCMD.CMD both open the FCB the CCP is supposed to have filled in,
 * and against an all-zero one they open a nameless file and print
 * "No 'SUB' File Present" / "CANNOT OPEN SOURCE".
 *
 * The rules asserted here are the ones src/ccp/ccp.c already
 * implements for native programs -- delim(), true_char(), fill_fcb() --
 * because a guest and a native program on the same machine should be
 * handed the same FCB for the same tail.
 */
static char fcbseg[65536];

static void bpfcb(const char *tail)
{
	static struct i86cmd c;
	static struct i86 m;

	memset(&c, 0, sizeof c);
	memset(&m, 0, sizeof m);
	memset(fcbseg, 0xee, 256);
	c.model = M_SMALL;
	m.sb[S_DS] = fcbseg;
	i86bpage(&c, &m, S_DS, (char *)tail);
}

/* One FCB: the drive byte and the 11 blank-padded name bytes. */
static void chkfcb(const char *what, int off, int drive, const char *name)
{
	char w[64];
	int i;

	sprintf(w, "%s drive", what);
	chk(w, (long)(fcbseg[off] & 0xff), (long)drive);
	for (i = 0; i < 11; i++) {
		sprintf(w, "%s [%d]", what, i);
		chk(w, (long)(fcbseg[off + 1 + i] & 0xff), (long)(name[i] & 0xff));
	}
}

static void t_fcb(void)
{
	/* The gate's own tail.  One blank-separated token, so the `='
	 * ends FCB1's name and FCB2 stays blank -- which is exactly why
	 * PIP parses the tail itself instead of using these. */
	bpfcb(" I86OUT.TXT=I86IN.TXT");
	chkfcb("fcb1 pip", 0x5c, 0, "I86OUT  TXT");
	chkfcb("fcb2 pip", 0x6c, 0, "           ");

	/* A bare name with no extension: the one SUBMIT is given, and
	 * the one an all-zero base page turned into a nameless file. */
	bpfcb(" I86SUB");
	chkfcb("fcb1 submit", 0x5c, 0, "I86SUB     ");

	/* Two tokens, two drives.  Drive 0 is the default drive, which
	 * is what DRI's CCP leaves for a guest to find. */
	bpfcb(" B:X.Y A:LONGNAME.EXTRA");
	chkfcb("fcb1 drive B", 0x5c, 2, "X       Y  ");
	chkfcb("fcb2 drive A", 0x6c, 1, "LONGNAMEEXT");

	/* `*' becomes `?' and is NOT consumed, so it fills its field. */
	bpfcb(" *.*");
	chkfcb("fcb1 star", 0x5c, 0, "???????????");
	bpfcb(" A*.C?D");
	chkfcb("fcb1 partial star", 0x5c, 0, "A???????C?D");

	/* Lower case folds up, the way the CCP folds the whole line. */
	bpfcb(" hello.h86");
	chkfcb("fcb1 lower case", 0x5c, 0, "HELLO   H86");

	/* No tail at all, and a drive with no name: both are blank
	 * names on the named drive, not garbage. */
	bpfcb("");
	chkfcb("fcb1 empty tail", 0x5c, 0, "           ");
	chkfcb("fcb2 empty tail", 0x6c, 0, "           ");
	bpfcb(" C:");
	chkfcb("fcb1 drive only", 0x5c, 3, "           ");

	/* The rest of FCB1 -- ex, s1, s2, rc at 0x68-0x6B -- must be
	 * zero, and the tail must still be where it was. */
	bpfcb(" I86SUB");
	chk("fcb1 ex", (long)(fcbseg[0x68] & 0xff), 0);
	chk("fcb1 rc", (long)(fcbseg[0x6b] & 0xff), 0);
	chk("tail length survived", (long)(fcbseg[0x80] & 0xff), 7);
	chk("tail text survived", (long)(fcbseg[0x81] & 0xff), ' ');
}

/* ---- the backend switch ---- */

#define SYS_REC	0		/* record the call, answer sysret	*/
#define SYS_CPM	1		/* the stub CP/M below			*/

static int sysmode = SYS_REC;
static int sysret;		/* what the recording backend answers	*/
static int slast_fn;		/* what it was last handed		*/
static i16 slast_val;
static char *slast_addr;
static int sncall;

static int stub(int fn, i16 val, char *addr);

int i86sys(fn, val, addr)
int fn;
i16 val;
char *addr;
{
	sncall++;
	slast_fn = fn;
	slast_val = val;
	slast_addr = addr;
	if (sysmode == SYS_CPM)
		return (stub(fn, val, addr));
	return (sysret);
}

/* ---- 8a: the mapping ---- */

static char bseg[65536];	/* the guest's data segment		*/
static char cseg[65536];	/* ... and its code segment		*/
static struct i86 bm;

/* What i86bdosinit() costs before a guest has asked for anything: the
 * console mode read and set, and the default DMA address. */
#define SINIT	3

/* A guest sitting at CS:0 with DS a segment of its own, which is the
 * shape i86place() gives a small-model .CMD. */
static void bsetup(void)
{
	memset(bseg, 0, sizeof bseg);
	memset(cseg, 0, sizeof cseg);
	memset(&bm, 0, sizeof bm);
	bm.sb[S_CS] = cseg;
	bm.sr[S_CS] = 0x1000;
	bm.sb[S_DS] = bm.sb[S_SS] = bm.sb[S_ES] = bseg;
	bm.sr[S_DS] = bm.sr[S_SS] = bm.sr[S_ES] = 0x2000;
	bm.lz = LZ_NONE;
	bm.fl = F_ONES;
	bm.ip = 0;
	bm.r[R_SP] = 0xff00;
	i86nseg = 2;
	i86spar[0] = 0x1000; i86sbase[0] = cseg;
	i86spar[1] = 0x2000; i86sbase[1] = bseg;
	/* The data group's own allocation, as i86place() would have left
	 * it: 0x800 paragraphs, which is what DRI's STAT.CMD asks for.
	 * The half above is the shim's. */
	i86dgpar = 0x2000;
	i86dgtop = 0x8000L;
	sysmode = SYS_REC;
	sysret = 0;
	sncall = 0;
	slast_fn = -1;
	slast_val = 0;
	slast_addr = 0;
	i86bdosinit(&bm);
}

/* Put CL and DX where a CP/M-86 program puts them, raise the interrupt
 * the way the guest's `int 0E0h' does, and service it. */
static int bcall(int fn, i16 dx)
{
	bm.r[R_CX] = (i16)fn;
	bm.r[R_DX] = dx;
	i86intno = 0xe0;
	return (i86bdos(&bm));
}

static void t_seam(void)
{
	int rc;

	/* The DMA address a program starts with: DS:0080, the base
	 * page's own buffer, set before the first instruction runs. */
	bsetup();
	chk("init dma calls", sncall, SINIT);
	chk("init dma fn", slast_fn, 26);
	ntest++;
	if (slast_addr != &bseg[0x80])
		fail("init dma addr", 1, 0);
	chk("init dma off", i86dmaoff & 0xffff, 0x80);
	chk("init dma seg", i86dmaseg & 0xffff, 0x2000);

	/* Byte parameter: DL, not DX -- and console output is
	 * COLLECTED, so DL goes into the batch and the native BDOS
	 * sees nothing until something flushes it.  That is the whole
	 * of the fn 111 change, asserted at the seam. */
	bsetup();
	chk("conout rc", bcall(2, (i16)0x4841), B_RUN);
	chk("conout reached no BDOS of its own", slast_fn, 26);
	chk("... and cost no gate crossing", sncall, SINIT);
	chk("conout flushes as one call", i86oflush(), 1);
	chk("... which is function 111", slast_fn, 111);
	chk("... with no value parameter", slast_val & 0xffff, 0);
	{
		struct sccb *c = (struct sccb *)slast_addr;

		chk("... a block of one character", c->n & 0xffff, 1);
		chk("... which is DL, not DX", c->a[0] & 0xff, 0x41);
	}
	chk("an empty batch flushes nothing", i86oflush(), 0);

	/* Anything else the guest asks for flushes first, so what the
	 * console shows stays in the order the guest wrote it. */
	bsetup();
	bcall(2, (i16)0x42);
	bcall(11, (i16)0);			/* console status	*/
	chk("a pending batch went out before the next function",
		slast_fn, 11);
	chk("... which is two calls, 111 then 11", sncall, SINIT + 2);

	/* Word parameter: the whole of DX. */
	bsetup();
	chk("reset drive rc", bcall(37, (i16)0x0003), B_RUN);
	chk("reset drive val", slast_val & 0xffff, 0x0003);

	/* The result lands in AL, AX and BX, and the high byte of a CP/M
	 * 3 error return survives -- CP/M-86 uses AH for the same thing. */
	bsetup();
	sysret = 0x09ff;
	bcall(20, (i16)0x100);
	chk("result ax", bm.r[R_AX] & 0xffff, 0x09ff);
	chk("result bx", bm.r[R_BX] & 0xffff, 0x09ff);

	/* An FCB goes by copy, holding the guest's bytes: the native BDOS
	 * writes back 36 of them and a sequential FCB is 33 (section 8h,
	 * ASM86). */
	bsetup();
	bseg[0x180] = 0x03;
	chk("open rc", bcall(15, (i16)0x180), B_RUN);
	chk("open fn", slast_fn, 15);
	ntest++;
	if (slast_addr == &bseg[0x180])
		fail("open addr", 1, 0);
	chk("open fcb copied", slast_addr[0] & 0xff, 0x03);

	/* ... and it is checked for length.  36 bytes at 0xFFDC fit;
	 * at 0xFFDD they do not, and the call never leaves. */
	bsetup();
	chk("fcb at limit", bcall(15, (i16)0xffdc), B_RUN);
	bsetup();
	chk("fcb past limit", bcall(15, (i16)0xffdd), B_ADDR);
	chk("fcb past limit no call", sncall, SINIT);	/* the init only */
	chk("fcb past limit fn", i86bdosfn, 15);

	/* Rename takes two FCBs in one block, and the check has to know
	 * that: 52 bytes, not 36. */
	bsetup();
	chk("rename at limit", bcall(23, (i16)0xffcc), B_RUN);
	bsetup();
	chk("rename past limit", bcall(23, (i16)0xffcd), B_ADDR);

	/* Function 9 scans for its own terminator before the native
	 * BDOS can scan past the end of the segment looking for it. */
	bsetup();
	strcpy(&bseg[0x200], "hello$");
	chk("printstr rc", bcall(9, (i16)0x200), B_RUN);
	ntest++;
	if (slast_addr != &bseg[0x200])
		fail("printstr addr", 1, 0);
	bsetup();
	memset(&bseg[0xff00], 'x', 0x100);	/* no `$' to the end	*/
	chk("printstr unterminated", bcall(9, (i16)0xff00), B_ADDR);

	/* A console buffer is as long as its own first byte says. */
	bsetup();
	bseg[0xfff0] = 13;			/* 13 + 2 = 15 > 16	*/
	chk("conbuf fits", bcall(10, (i16)0xfff0), B_RUN);
	bsetup();
	bseg[0xfff0] = 14;			/* 14 + 2 = 16, exactly	*/
	chk("conbuf exact", bcall(10, (i16)0xfff0), B_RUN);
	bsetup();
	bseg[0xfff0] = 15;
	chk("conbuf past limit", bcall(10, (i16)0xfff0), B_ADDR);

	/* Function 12 is answered here and never reaches the native
	 * BDOS, which reports 0x2031 -- CP/M 3, a level no 1982 .CMD has
	 * seen.  K4, and PLAN.md D1 is the same failure from the other
	 * side.  The high byte is the machine type and must be zero:
	 * DRI's TOD.CMD refuses every non-zero one (src/cmd/i86bdos.c
	 * i86ver). */
	bsetup();
	sysret = 0x2031;
	chk("version rc", bcall(12, (i16)0), B_RUN);
	chk("version ax", bm.r[R_AX] & 0xffff, 0x0022);
	chk("version bx", bm.r[R_BX] & 0xffff, 0x0022);
	chk("version no call", sncall, SINIT);		/* the init only */

	/* Function 0 is the guest terminating.  It must NOT reach our
	 * function 0, which is warmboot() and does not return. */
	bsetup();
	chk("reset rc", bcall(0, (i16)0), B_EXIT);
	chk("reset no call", sncall, SINIT);

	/* The DMA address, in the two halves CP/M-86 splits it into.
	 * Setting the offset keeps the base; setting the base keeps the
	 * offset; either one re-issues ONE native call. */
	bsetup();
	chk("dma off rc", bcall(26, (i16)0x400), B_RUN);
	chk("dma off fn", slast_fn, 26);
	ntest++;
	if (slast_addr != &bseg[0x400])
		fail("dma off addr", 1, 0);
	chk("dma off calls", sncall, SINIT + 1);
	chk("dma base rc", bcall(51, (i16)0x1000), B_RUN);
	ntest++;
	if (slast_addr != &cseg[0x400])
		fail("dma base addr", 1, 0);

	/* A DMA base we never handed out is K3's finding arriving
	 * through the other door. */
	bsetup();
	chk("dma base unknown", bcall(51, (i16)0xb800), B_SEG);
	chk("dma base reported", i86segbad & 0xffff, 0xb800);

	/* A DMA buffer that would run off the end of the segment is
	 * refused before the BDOS writes 128 bytes into the next one. */
	bsetup();
	chk("dma off at limit", bcall(26, (i16)0xff80), B_RUN);
	bsetup();
	chk("dma off past limit", bcall(26, (i16)0xff81), B_ADDR);

	/* The functions stage one refuses by name, each for a reason in
	 * the file's own comment: two are MP/M's, and the sized memory
	 * calls have nothing honest to answer with while allocation is a
	 * whole segment.  27, 31, 49, 57 and 59 are not among them any
	 * more -- see sections 8e and 8f. */
	bsetup(); chk("fn 38 refused", bcall(38, (i16)0), B_FN);
	bsetup(); bseg[0] = 9; chk("fn 50 SELDSK refused", bcall(50, (i16)0),
	    B_FN);
	bsetup(); chk("fn 52 refused", bcall(52, (i16)0), B_FN);
	bsetup(); chk("fn 53 refused", bcall(53, (i16)0), B_FN);
	bsetup(); chk("fn 56 refused", bcall(56, (i16)0), B_FN);
	chk("refused fn recorded", i86bdosfn, 56);

	/* Function 6: FF answers a key or 0 without waiting, FE the
	 * status, anything else -- FD included -- is output. */
	bsetup();
	chk("fn 6 FF no key rc", bcall(6, (i16)0xff), B_RUN);
	chk("fn 6 FF no key ax", bm.r[R_AX] & 0xffff, 0);
	chk("fn 6 FF no key asked status", slast_val & 0xffff, 0xfe);
	chk("fn 6 FF no key did not wait", sncall, SINIT + 1);
	bsetup();
	sysret = 'K';
	bcall(6, (i16)0xff);
	chk("fn 6 FF key ax", bm.r[R_AX] & 0xffff, 'K');
	chk("fn 6 FF key read", slast_val & 0xffff, 0xff);
	chk("fn 6 FF key calls", sncall, SINIT + 2);
	bsetup();
	sysret = 1;
	bcall(6, (i16)0xfe);
	chk("fn 6 FE ready", bm.r[R_AX] & 0xffff, 0xff);
	bsetup();
	bcall(6, (i16)0xfe);
	chk("fn 6 FE idle", bm.r[R_AX] & 0xffff, 0);
	bsetup();
	bcall(6, (i16)0xfd);
	chk("fn 6 FD is output", slast_val & 0xffff, 0xfd);

	/* Function 50's console vectors, from the block at DS:DX. */
	bsetup();
	bseg[0x40] = 4;
	bseg[0x41] = 'z';
	chk("fn 50 CONOUT rc", bcall(50, (i16)0x40), B_RUN);
	chk("fn 50 CONOUT fn", slast_fn, 6);
	chk("fn 50 CONOUT char", slast_val & 0xffff, 'z');
	bsetup();
	sysret = 'Q';
	bseg[0x40] = 3;
	chk("fn 50 CONIN rc", bcall(50, (i16)0x40), B_RUN);
	chk("fn 50 CONIN ax", bm.r[R_AX] & 0xffff, 'Q');
	chk("fn 50 CONIN read", slast_val & 0xffff, 0xff);
	bsetup();
	sysret = 1;
	bseg[0x40] = 2;
	bcall(50, (i16)0x40);
	chk("fn 50 CONST ready", bm.r[R_AX] & 0xffff, 0xff);
	chk("fn 50 CONST fn", slast_fn, 11);
	bsetup();
	chk("fn 50 block off the segment", bcall(50, (i16)0xfffc), B_ADDR);

	/* Any other interrupt is a refusal that says which, and vector 0
	 * -- the divide error i86exec.c raises -- is distinguished from
	 * a vector the guest asked for. */
	bsetup();
	i86intno = 0x21;
	chk("foreign vector", i86bdos(&bm), B_VEC);
	i86intno = 0;
	chk("divide error", i86bdos(&bm), B_TRAP);
	i86intno = 0xe0;

	/* The whole thing driven by a real INT instruction rather than
	 * by setting i86intno: MOV CL,2 / MOV DL,'Z' / INT 0E0h. */
	bsetup();
	cseg[0] = (char)0xb1; cseg[1] = 0x02;		/* mov cl,2	*/
	cseg[2] = (char)0xb2; cseg[3] = 'Z';		/* mov dl,'Z'	*/
	cseg[4] = (char)0xcd; cseg[5] = (char)0xe0;	/* int 0e0h	*/
	{
		struct i86in in;
		chk("int step 1", i86step(&bm, &in), X_OK);
		chk("int step 2", i86step(&bm, &in), X_OK);
		rc = i86step(&bm, &in);
		chk("int step 3", rc, X_INT);
		chk("int vector", i86intno, 0xe0);
		chk("int ip past", bm.ip & 0xffff, 6);
		chk("int serviced", i86bdos(&bm), B_RUN);
		chk("int flushes as one call", i86oflush(), 1);
		chk("int fn", slast_fn, 111);
		chk("int val", ((struct sccb *)slast_addr)->a[0] & 0xff,
			'Z');
	}
}

/* ---- 8b: a stub CP/M, and DRI's PIP running on it ---- */

/*
 * Eight files, each a name and a byte count, all in memory.  This is
 * not a filesystem: it is the smallest thing that answers the calls a
 * copy makes, so that the ANSWER can be checked.  Everything it does
 * not implement returns 0xFF and is counted, and the counts are
 * printed -- an unimplemented call that PIP relies on shows up as a
 * failed copy plus a number saying which function it was.
 */
#define SF_MAX	8
#define SF_CAP	32768

struct sfile {
	char	name[11];	/* 8 + 3, blank padded, upper case	*/
	int	used;
	long	len;		/* bytes; CP/M rounds to 128		*/
	char	d[SF_CAP];
};

static struct sfile sdisk[SF_MAX];
static char *sdma;		/* the native "DMA address"		*/
static char sbase[128];		/* where function 13 leaves it		*/
static long sfncount[113];	/* every function the seam reached	*/
static char scon[8192];		/* the guest's console output		*/
static int sconn;
static int ssearch;		/* search-next cursor			*/
static char ssname[11];

/*
 * The console the real BDOS presents, flow control included.
 *
 * conbrk() polls every eight output characters and CONSUMES what it
 * finds: ^S/^Q are swallowed and anything else is held in kbchar, one
 * byte per console, so a second key typed during output overwrites the
 * first.  CM_NOSTOP makes it return before reading.  Modelling that is
 * the point -- a stub whose output never touched the keys could not
 * tell the two modes apart.
 */
#define SKQ	64			/* keys waiting in the BIOS	*/

static int sconmode;			/* function 109's word		*/
static unsigned char skq[SKQ];
static int skqh, skqt;
static int skbchar;			/* the BDOS's one byte of it	*/
static int sbrkctr;

static int skqon;			/* keys have been typed at this run */

static void skqput(const char *s)
{
	skqon = 1;
	while (*s && skqt < SKQ)
		skq[skqt++] = (unsigned char)*s++;
}

static void sconbrk(void)
{
	if (sconmode & CM_NOSTOP) {
		sbrkctr = 0;
		return;
	}
	if (++sbrkctr < 8)
		return;
	sbrkctr = 0;
	while (skqh < skqt) {
		int c = skq[skqh++];

		if (c == 0x13 || c == 0x11)	/* ^S, ^Q		*/
			continue;
		skbchar = c;			/* and the one before it
						   is gone		*/
		break;
	}
}

static int skqget(void)			/* function 6, E = FFh		*/
{
	if (skbchar) {
		int c = skbchar;

		skbchar = 0;
		return (c);
	}
	if (skqh < skqt)
		return (skq[skqh++]);
	return (0);
}

static void sputc(int c)
{
	sconbrk();
	if (sconn < (int)sizeof scon - 1)
		scon[sconn++] = (char)c;
}

/*
 * Scripted console input, for the `-x'/`-X' runs only: a program that
 * reads a command line and acts on it cannot be watched doing it
 * otherwise.  Null means there is no script and the console functions
 * are left exactly as they were, which is what every corpus run uses.
 * A newline in the script is the carriage return a CP/M program is
 * waiting for; running out sets skeyeof, and runx() stops rather than
 * let a command loop spin to the step limit.
 */
static const char *skeys;
static int skeyeof;
static int swboot;		/* function 10 read a ^C		*/

static int skey(void)
{
	int c;

	if (skeys == 0 || *skeys == 0) {
		skeyeof = 1;
		return (-1);
	}
	c = *skeys++ & 0xff;
	return (c == '\n' ? '\r' : c);
}

/* An FCB name is 11 bytes with the high bits used as attributes; a
 * comparison has to mask them, and a search has to honour `?'. */
static int smatch(const char *a, const char *b, int wild)
{
	int i;

	for (i = 0; i < 11; i++) {
		if (wild && b[i] == '?')
			continue;
		if ((a[i] & 0x7f) != (b[i] & 0x7f))
			return (0);
	}
	return (1);
}

static struct sfile *sfind(const char *nm)
{
	int i;

	for (i = 0; i < SF_MAX; i++)
		if (sdisk[i].used && smatch(sdisk[i].name, nm, 0))
			return (&sdisk[i]);
	return (0);
}

static struct sfile *smake(const char *nm)
{
	int i;

	for (i = 0; i < SF_MAX; i++)
		if (!sdisk[i].used) {
			memcpy(sdisk[i].name, nm, 11);
			sdisk[i].used = 1;
			sdisk[i].len = 0;
			return (&sdisk[i]);
		}
	return (0);
}

/* The record a sequential call is at: extent * 128 + current record. */
/*
 * Write an FCB's random record field the way OUR BDOS writes it, which
 * is not the way an 8086's does: r0 at offset 33 is the HIGH byte
 * (src/bdos/fileio.c setran/fsize, "the same big-endian bytes").  This
 * stub had it little-endian -- the GUEST's order -- so it cancelled
 * against the seam's missing translation and the two ends agreed here
 * while they would have disagreed on the machine.  The Z80 lane's gate
 * caught exactly that, on the emulator, after months of the same host
 * run passing (src/cmd/i86bdos.c ranswap).
 */
static void sranset(char *f, long r)
{
	f[33] = (char)((r >> 16) & 1);
	f[34] = (char)((r >> 8) & 0xff);
	f[35] = (char)(r & 0xff);
}

static long srec(const char *f)
{
	return ((long)(f[12] & 0x1f) * 128L + (long)(f[32] & 0x7f));
}

static void sbump(char *f)
{
	int cr;

	cr = (f[32] & 0x7f) + 1;
	if (cr > 127) {
		cr = 0;
		f[12] = (char)((f[12] & 0xff) + 1);
	}
	f[32] = (char)cr;
}

/*
 * The random record field, read the way OUR BDOS reads it -- ran0
 * (fcb+33) is the HIGH byte, matching sranset() above.  A guest's own
 * bytes are in the OTHER order (fcb+33 low); i86bdos.c's ranswap() is
 * what makes that true by the time this stub ever sees the FCB
 * (src/bdos/fileio.c setran/fsize).
 */
static long srrec(const char *f)
{
	return (((long)(f[33] & 0xff) << 16)
	      | ((long)(f[34] & 0xff) << 8)
	      | (long)(f[35] & 0xff));
}

/*
 * Advance the random record field by one, in the SAME (post-ranswap)
 * byte order srrec() reads -- src/bdos/bdosrw.c incr_rr() to the
 * letter: ran2 (here fcb+35, the low byte) carries into ran1, then
 * ran0.
 */
static void srincr(char *f)
{
	int t;

	t = (f[35] & 0xff) + 1;
	f[35] = (char)(t & 0xff);
	if (t <= 0xff)
		return;
	t = (f[34] & 0xff) + 1;
	f[34] = (char)(t & 0xff);
	if (t <= 0xff)
		return;
	f[33] = (char)((f[33] & 0xff) + 1);
}

/*
 * One record, sequential: src/bdos/bdosrw.c bdosrw()'s sequential arm
 * flattened onto this stub's flat files, lifted out of the switch below
 * so that multio() has something to loop on.
 */
static int srw1(int fn, char *addr)
{
	struct sfile *f;
	long r, n;

	f = sfind(addr + 1);
	if (!f)
		return (9);
	r = srec(addr);
	if (fn == 20) {
		if (r * 128L >= f->len)
			return (1);		/* end of file		*/
		n = f->len - r * 128L;
		if (n > 128)
			n = 128;
		memset(sdma, 0x1a, 128);
		memcpy(sdma, f->d + r * 128L, (size_t)n);
	} else {
		if ((r + 1) * 128L > (long)SF_CAP)
			return (2);		/* disk full		*/
		memcpy(f->d + r * 128L, sdma, 128);
		if ((r + 1) * 128L > f->len)
			f->len = (r + 1) * 128L;
	}
	sbump(addr);
	return (0);
}

/*
 * One record, RANDOM, at the record the FCB's own random-record field
 * names -- src/bdos/bdosrw.c bdosrw()'s random arm, flattened the same
 * way srw1() flattens the sequential one: no extents, no blocks, just
 * an absolute record number into the stub's flat file.
 *
 * The 0x40000 test is new_ext()'s `if (mod >= 64) return(6)'
 * (src/bdos/bdosrw.c:136) in flat form.  A module is 32 extents of 128
 * records, so module 64 begins at record 64 * 32 * 128 = 0x40000, and
 * that is the one record number the real BDOS refuses before it has
 * looked at the file at all -- a different answer from code 1, which
 * means "this file does not go that far yet".
 *
 * fn 40, write random WITH ZERO FILL, is the one place this stub's
 * flat model has to say something the real BDOS says at a different
 * layer.  On the real machine the zero-fill is a per-BLOCK guarantee
 * (bdosrw.c: a newly allocated block is zeroed through the directory
 * buffer before the caller's record is written into it), so records
 * inside the same block that the caller never writes read back as zero
 * rather than as leftover disk content.  This stub has no block layer
 * -- the file is a flat array -- so the equivalent guarantee is made at
 * the RECORD level: writing past the current end of file zeros the gap
 * in the array first.  That is a smaller promise than the real BDOS
 * makes (it zeros to the next block boundary, not just to the record
 * being written) but it is the same promise on every case this
 * project's tests can observe -- a read of any record between the old
 * EOF and the new one -- and it is why fn 40 is the one of the three
 * that needs its own branch below rather than sharing fn 34's.
 */
static int ranw1(int fn, char *addr)
{
	struct sfile *f;
	long r, n;

	f = sfind(addr + 1);
	if (!f)
		return (9);
	r = srrec(addr);
	if (r >= 0x40000L)
		return (6);		/* past maximum file size	*/
	/* A sequential call after this one starts at the same record. */
	addr[12] = (char)((r >> 7) & 0x1f);
	addr[32] = (char)(r & 0x7f);
	if (fn == 33) {				/* read random		*/
		if (r * 128L >= f->len)
			return (1);		/* reading unwritten data */
		n = f->len - r * 128L;
		if (n > 128)
			n = 128;
		memset(sdma, 0x1a, 128);
		memcpy(sdma, f->d + r * 128L, (size_t)n);
	} else {				/* write random, 34 or 40 */
		if ((r + 1) * 128L > (long)SF_CAP)
			return (2);		/* disk full		*/
		if (fn == 40 && r * 128L > f->len)
			memset(f->d + f->len, 0, (size_t)(r * 128L - f->len));
		memcpy(f->d + r * 128L, sdma, 128);
		if ((r + 1) * 128L > f->len)
			f->len = (r + 1) * 128L;
	}
	return (0);
}

/*
 * MULTI-SECTOR I/O, src/bdos/bdosrw.c multio() to the letter -- the DMA
 * address advances by one record between transfers and is restored on
 * exit, and the high byte of a non-physical failure is the number of
 * records that got through.
 *
 * It is here because BDOS FUNCTION 44 USED TO FALL THROUGH TO THIS
 * STUB'S `default: return 0xff'.  A stub that REFUSES a call the real
 * BDOS accepts hides whatever the accepted call would have done -- here,
 * that our BDOS writes count * 128 bytes from the DMA address, which is
 * the whole of what the seam's DMA check has to cover.  tests/verify.mk
 * verify-z80pip records the same lesson from the other direction: the
 * Z80 stub ACCEPTED function 44 and ignored it, and the two machines
 * then disagreed about a program behaving correctly on both.
 *
 * It covers all five functions the real multio() shells -- 20 and 21
 * sequential, 33/34/40 random -- and for the random three it also
 * advances and restores the FCB's own random-record field exactly as
 * incr_rr() and multio() do: one step per record transferred, the whole
 * field put back to the caller's value before returning.  Sequential
 * I/O advances the FCB's CURRENT-RECORD byte instead (sbump(), inside
 * srw1()), which is why only the random arm touches the record field.
 */
static int smultcnt = 1;		/* BDOS function 44's count	*/

static int smultio(int fn, char *addr)
{
	char *sav_dma;
	char sav33, sav34, sav35;
	int done, rtn, isran;

	isran = (fn == 33 || fn == 34 || fn == 40);

	if (smultcnt <= 1)
		return (isran ? ranw1(fn, addr) : srw1(fn, addr));

	sav_dma = sdma;
	sav33 = sav34 = sav35 = 0;
	if (isran) {
		sav33 = addr[33];
		sav34 = addr[34];
		sav35 = addr[35];
	}
	done = 0;
	rtn = 0;
	while (done < smultcnt) {
		rtn = isran ? ranw1(fn, addr) : srw1(fn, addr);
		if (rtn != 0)
			break;
		done++;
		if (isran)
			srincr(addr);
		sdma += 128;
	}
	sdma = sav_dma;
	if (isran) {
		addr[33] = sav33;
		addr[34] = sav34;
		addr[35] = sav35;
	}
	if (rtn == 0)
		return (0);
	if ((rtn & 0xff) == 0xff)
		return (rtn);
	return ((done << 8) | (rtn & 0xff));
}

/*
 * Drive A: as src/bios/boot900.c drvinit() builds it for the 10 MB
 * partition, and the same numbers tests/z80test.c holds: BLS 4096 (bsh
 * 5, blm 31) over 20,480 512-byte blocks, so dsm is 20480/8 - 1 and exm
 * is 1 because dsm is over 255.  DRM 511 with four directory blocks
 * reserved, a fixed disk (cks 0), no system tracks.
 */
static struct gdpb sdpb = { 64, 5, 31, 1, 0, 2559, 511, 0xF000, 0, 0 };

/* The vector a login scan leaves on an empty drive: the four directory
 * blocks and nothing else, block 0 in the TOP bit of the first byte
 * (src/bdos/dskutil.c setaloc). */
static char salv[(2559 >> 3) + 1] = { (char)0xf0 };
static char sclk[5];		/* day count, BCD hour, minute, second	*/

/* The extent, record count and 4 KB block numbers of file `i', as its one
 * directory entry would hold them: 256 records per entry at EXM 1, so
 * every stub file fits in one.  Blocks are 8 apart per file, after the
 * four the directory takes. */
static void sdirent(char *e, int i)
{
	long r;
	int b;

	r = (sdisk[i].len + 127) / 128;
	e[12] = (char)(r > 128 ? 1 : 0);
	e[15] = (char)(r > 128 ? r - 128 : r);
	for (b = 0; b < (int)((r + 31) / 32); b++) {
		e[16 + 2 * b] = (char)(4 + 8 * i + b);
		e[17 + 2 * b] = 0;
	}
}

static int stub1(int fn, i16 val, char *addr);

/* Our BDOS works on a 36-byte copy of an FCB and writes all of it back
 * when the call ends, after any transfer into the DMA buffer.  Search
 * next ignores its parameter and uses search first's FCB. */
static int stub(int fn, i16 val, char *addr)
{
	static char *srchp;
	char t[36];
	int r;

	if (!((fn >= 15 && fn <= 23) || fn == 30 || (fn >= 33 && fn <= 36)
	 || fn == 40))
		return (stub1(fn, val, addr));
	if (fn == 17)
		srchp = addr;
	if (fn == 18 && srchp)
		addr = srchp;
	memcpy(t, addr, sizeof t);
	r = stub1(fn, val, t);
	memcpy(addr, t, sizeof t);
	return (r);
}

static int stub1(int fn, i16 val, char *addr)
{
	struct sfile *f;
	long r, n;
	int i;

	if (fn >= 0 && fn < (int)(sizeof sfncount / sizeof sfncount[0]))
		sfncount[fn]++;
	switch (fn) {
	case 2:					/* console output	*/
		sputc(val & 0x7f);
		return (0);
	case 9:					/* print string		*/
		for (i = 0; addr[i] != '$' && i < 4096; i++)
			sputc(addr[i] & 0x7f);
		return (0);
	case 111: {				/* print block to console */
		/* The native character control block the seam builds
		 * for a batch of function 2s: {address, count}, with
		 * the address a host pointer because that is what an
		 * XADDR is on the target (src/cmd/cpm.h:5-13).  Our
		 * function 111 is prt_blk() -> cookdrun(), which is
		 * cookdout(ch, FALSE) per character -- the same thing
		 * function 2 above does -- so the record it leaves in
		 * scon[] has to be the same bytes in the same order. */
		struct sccb *c;

		c = (struct sccb *)addr;
		for (i = 0; i < (int)c->n; i++)
			sputc(c->a[i] & 0x7f);
		return (0);
	}
	case 1:					/* console input	*/
		if (!skeys)
			return (0xff);
		i = skey();
		if (i < 0)
			return (0x1a);
		sputc(i);
		return (i);
	case 10: {				/* read console buffer	*/
		int max, k, c;

		if (!skeys)
			return (0xff);
		/* ^C first on the line is the BDOS's warm boot. */
		if (*skeys == 3) {
			skeys++;
			swboot = 1;
			sputc('^');
			sputc('C');
			addr[1] = 0;
			return (0);
		}
		max = addr[0] & 0xff;
		k = 0;
		while ((c = skey()) >= 0 && c != '\r' && k < max)
			addr[2 + k++] = (char)c;
		addr[1] = (char)k;
		for (i = 0; i < k; i++)
			sputc(addr[2 + i]);
		sputc('\r');
		sputc('\n');
		return (0);
	}
	case 6:					/* direct console i/o	*/
		if (!skqon)
			return (0xff);		/* the corpus runs' answer */
		if ((val & 0xff) == 0xff)
			return (skqget());
		if ((val & 0xff) == 0xfe)
			return (skbchar || skqh < skqt ? 1 : 0);
		sputc(val & 0x7f);
		return (0);
	case 11:				/* console status	*/
		/* A scripted line is an answer to a read, never the
		 * keypress a program polls for to abort a listing, so
		 * only typed keys are reported here. */
		return (skbchar || skqh < skqt ? 1 : 0);
	case 109:				/* get/set console mode	*/
		if ((val & 0xffff) == 0xffff)
			return (sconmode);
		sconmode = val & 0xffff;
		return (0);
	case 12:
		return (0x2031);
	case 13:			/* reset disk system: the DMA goes
					 * back to the CALLER's base page */
		sdma = sbase;
		return (0);
	case 14: case 28: case 37:
		return (0);
	case 25:				/* current disk = A:	*/
		return (0);
	case 24:				/* login vector		*/
		return (1);
	case 29:				/* read-only vector	*/
		return (0);
	case 32:				/* get/set user code	*/
		return (val == 0xff ? 0 : 0);
	case 26:				/* set DMA address	*/
		sdma = addr;
		return (0);
	case 31:			/* copy out the disk parameters	*/
		memcpy(addr, &sdpb, sizeof sdpb);
		return (0);
	case 27:			/* copy out the allocation vector */
		memcpy(addr, salv, (size_t)((sdpb.dsm >> 3) + 1));
		return (0);
	case 46: {			/* free space on a drive	*/
		/* src/bdos/fileio.c free_sp(): the free blocks of the
		 * vector above, times the records in one, as three
		 * little-endian bytes and a zero in the DMA buffer. */
		long recs;
		int b;

		for (b = 0, recs = 0; b <= (int)sdpb.dsm; b++)
			if (!(salv[b >> 3] & (0x80 >> (b & 7))))
				recs += (long)sdpb.blm + 1L;
		if (sdma) {
			sdma[0] = (char)(recs & 0xff);
			sdma[1] = (char)((recs >> 8) & 0xff);
			sdma[2] = (char)((recs >> 16) & 0xff);
			sdma[3] = 0;
		}
		return (0);
	}
	case 15:				/* open			*/
		f = sfind(addr + 1);
		if (!f)
			return (0xff);
		n = (f->len + 127) / 128 - (long)(addr[12] & 0x1f) * 128L;
		if (n < 0)
			n = 0;
		if (n > 128)
			n = 128;
		addr[15] = (char)n;		/* record count		*/
		return (0);
	case 22:				/* make			*/
		f = sfind(addr + 1);
		if (f)
			f->used = 0;
		f = smake(addr + 1);
		if (!f)
			return (0xff);
		addr[12] = 0;
		addr[15] = 0;
		return (0);
	case 16:				/* close			*/
		return (0);
	case 30:				/* set file attributes		*/
		return (0);
	case 23:				/* rename: old at 1, new at 17	*/
		f = sfind(addr + 1);
		if (!f)
			return (0xff);
		memcpy(f->name, addr + 17, 11);
		return (0);
	case 35:				/* compute file size		*/
		f = sfind(addr + 1);
		if (!f)
			return (0xff);
		r = (f->len + 127) / 128;
		sranset(addr, r);
		return (0);
	case 36:				/* set random record		*/
		r = srec(addr);
		sranset(addr, r);
		return (0);
	case 19:				/* delete		*/
		for (i = 0, n = 0; i < SF_MAX; i++)
			if (sdisk[i].used && smatch(sdisk[i].name, addr + 1, 1)) {
				sdisk[i].used = 0;
				n++;
			}
		return (n ? 0 : 0xff);
	case 17:				/* search first		*/
		ssearch = 0;
		/* fall through */
	case 18:				/* search next		*/
		/* Drive `?' asks for every entry, whatever the name. */
		memset(ssname, '?', 11);
		if (addr[0] != '?')
			memcpy(ssname, addr + 1, 11);
		for (i = ssearch; i < SF_MAX; i++)
			if (sdisk[i].used && smatch(sdisk[i].name, ssname, 1)) {
				ssearch = i + 1;
				if (sdma) {
					memset(sdma, 0, 32);
					memcpy(sdma + 1, sdisk[i].name, 11);
					sdirent(sdma, i);
				}
				return (0);
			}
		ssearch = SF_MAX;
		return (0xff);
	case 20:				/* read sequential	*/
	case 21:				/* write sequential	*/
	case 33:				/* read random		*/
	case 34:				/* write random		*/
	case 40:				/* write random, 0 fill	*/
		return (smultio(fn, addr));
	case 44:				/* set multi-sector count */
		/* src/bdos/bdosmain.c:612-616 exactly: 0 and >128 are
		 * refused and the count is left alone. */
		i = val & 0xff;
		if (i == 0 || i > 128)
			return (0xff);
		smultcnt = i;
		return (0);
	case 104:			/* set date and time: seconds to 0 */
		memcpy(sclk, addr, 4);
		sclk[4] = 0;
		return (0);
	case 105:				/* get date and time	*/
		memcpy(addr, sclk, 4);
		return (sclk[4] & 0xff);
	default:
		return (0xff);
	}
}

/* Turn "VERIFY  IN " out of "VERIFY.IN". */
static void smkname(char *out, const char *s)
{
	int i;

	memset(out, ' ', 11);
	for (i = 0; i < 8 && *s && *s != '.'; i++)
		out[i] = *s++;
	while (*s && *s != '.')
		s++;
	if (*s == '.')
		s++;
	for (i = 0; i < 3 && *s; i++)
		out[8 + i] = *s++;
}

/* ---- 8e: the two functions whose answer is an address ---- */

/*
 * The seventeen bytes drive A: comes to.  tests/z80test.c holds the same
 * list for the CP/M-80 seam, and the two have to agree: one disk, one
 * block, whichever guest is asking.  A CP/M 2.2 guest, which is what
 * this one is told it is talking to, reads the first fifteen and stops.
 */
static const unsigned char edpb[GDPB_LEN] = {
	0x40, 0x00,		/* SPT 64 records a track		*/
	0x05,			/* BSH: 4096-byte blocks		*/
	0x1f,			/* BLM					*/
	0x01,			/* EXM: dsm is over 255			*/
	0xff, 0x09,		/* DSM 2559				*/
	0xff, 0x01,		/* DRM 511				*/
	0xf0, 0x00,		/* AL0, AL1: four directory blocks	*/
	0x00, 0x00,		/* CKS 0: a fixed disk			*/
	0x00, 0x00,		/* OFF 0: no system tracks		*/
	0x00,			/* PSH: the BIOS takes 128-byte records	*/
	0x00			/* PHM					*/
};

#define EALV	((2559 >> 3) + 1)	/* the vector's length in bytes	*/

static void t_dparms(void)
{
	int i, bad;

	bsetup();
	sysmode = SYS_CPM;
	chk("fn 31 is answered", bcall(31, (i16)0), B_RUN);
	chk("... with an offset in BX", bm.r[R_BX] & 0xffff, 0xffe0);
	chk("... the same in AX", bm.r[R_AX] & 0xffff, 0xffe0);
	chk("... and the group's paragraph in ES", bm.sr[S_ES] & 0xffff,
		0x2000);
	chk("... above the memory the guest was given",
		0xffe0L >= i86dgtop, 1);
	for (i = 0, bad = -1; i < GDPB_LEN; i++)
		if ((bseg[0xffe0 + i] & 0xff) != edpb[i])
			bad = i;
	chk("... holding the disk parameter block of drive A:", bad, -1);

	chk("fn 27 is answered", bcall(27, (i16)0), B_RUN);
	chk("... with the vector below the block", bm.r[R_BX] & 0xffff,
		0xffe0 - EALV);
	chk("... still above the guest's own memory",
		(long)(0xffe0L - EALV) >= i86dgtop, 1);
	chk("... four directory blocks allocated",
		bseg[0xffe0 - EALV] & 0xff, 0xf0);
	chk("... and nothing after them",
		bseg[0xffe0 - EALV + 1] & 0xff, 0);
	chk("... to the last byte of the vector",
		bseg[0xffdf] & 0xff, 0);

	/* A guest whose group filled its whole 64 KB leaves nowhere to
	 * put either block, and gets the refusal it got before. */
	bsetup();
	sysmode = SYS_CPM;
	i86dgtop = 0x10000L;
	chk("no room, fn 31 refused", bcall(31, (i16)0), B_FN);
	chk("no room, fn 27 refused", bcall(27, (i16)0), B_FN);

	sysmode = SYS_REC;
}

/*
 * BDOS functions 59 and 57: the program a guest loads, and gives back.
 *
 * The guest here is bsetup()'s synthetic small-model program -- two
 * segments and nothing else in the pool -- and the program it loads is
 * DRI's own PIP.CMD, read off the corpus into the stub's file system.
 * That combination is the point: the same loader the gate runs at
 * startup, driven from behind the seam by a guest's FCB, against a real
 * header.
 */
static int lsheld(void)
{
	int i, n;

	for (i = n = 0; i < CMD_NGRP; i++)
		if (lstaken[i])
			n++;
	return (n);
}

static void t_pload(const char *dir)
{
	char path[512];
	struct sfile *fp;
	FILE *f;
	int i;

	sprintf(path, "%s/PIP.CMD", dir);
	f = fopen(path, "rb");
	if (f == 0) {
		printf("i86test: %s unreadable -- section 8f skipped\n", path);
		return;
	}

	bsetup();
	sysmode = SYS_CPM;
	memset(sdisk, 0, sizeof sdisk);
	fp = &sdisk[0];
	smkname(fp->name, "PIP.CMD");
	fp->used = 1;
	fp->len = (long)fread(fp->d, 1, sizeof fp->d, f);
	fclose(f);
	memset(lstaken, 0, sizeof lstaken);
	i86segget = hsegget;
	i86segput = hsegput;

	/* The guest's FCB, where a base page keeps the first one. */
	memcpy(&bseg[0x5c + 1], "PIP     CMD", 11);

	/* The pool holds 0x1000 and 0x2000, so the program's two groups
	 * are the next two paragraphs up, and the base page is in the
	 * data group -- which is what the answer names. */
	chk("fn 59 answered", bcall(59, (i16)0x5c), B_RUN);
	chk("fn 59 base page", bm.r[R_AX] & 0xffff, 0x4000);
	chk("fn 59 answers BX too", bm.r[R_BX] & 0xffff, 0x4000);
	chk("fn 59 took two segments", lsheld(), 2);
	chk("fn 59 grew the pool", i86nseg, 4);
	chk("fn 59 code paragraph", i86spar[2] & 0xffff, 0x3000);
	chk("fn 59 data paragraph", i86spar[3] & 0xffff, 0x4000);

	/* The base page says where the groups are, and the groups ARE
	 * there: PIP's own first instruction is at the code paragraph
	 * the table names, and DS:0 is the base page itself. */
	chk("fn 59 base page code base",
		(i86sbase[3][3] & 0xff) | ((i86sbase[3][4] & 0xff) << 8),
		0x3000);
	chk("fn 59 base page data base",
		(i86sbase[3][9] & 0xff) | ((i86sbase[3][10] & 0xff) << 8),
		0x4000);
	ntest++;
	if (memcmp(i86resolve((i16)0x3000), "\234X\372\214\331", 5) != 0)
		fail("fn 59 code image at the paragraph named", 1, 0);
	/* The data group's image, not its base page: the 256 bytes the
	 * base page occupies are the loader's, the rest is the file's. */
	ntest++;
	if (memcmp(i86resolve((i16)0x4000) + 0x100, fp->d + 128
		+ 379L * 16 + 0x100, 16) != 0)
		fail("fn 59 data image behind the base page", 1, 0);

	/* A second load over the first takes no more memory: the guest
	 * keeps its own books, and an `E' repeated must not spend a
	 * segment either way. */
	chk("second fn 59 answered", bcall(59, (i16)0x5c), B_RUN);
	chk("second fn 59 base page", bm.r[R_AX] & 0xffff, 0x4000);
	chk("second fn 59 leaked nothing", lsheld(), 2);
	chk("second fn 59 left the pool alone", i86nseg, 4);

	/* Function 57.  The MCB's base is zero, which is CP/M-86's "all
	 * of it", and all of it is the program. */
	memset(&bseg[0x1400], 0, 5);
	bseg[0x1404] = (char)0xff;
	chk("fn 57 answered", bcall(57, (i16)0x1400), B_RUN);
	chk("fn 57 gave the segments back", lsheld(), 0);
	chk("fn 57 shortened the pool", i86nseg, 2);
	chk("fn 57 with nothing loaded", bcall(57, (i16)0x1400), B_RUN);

	/* No segment left: the load is refused with 0FFFFh in both of
	 * CP/M-86's result places, and nothing is half-acquired. */
	for (i = 0; i < CMD_NGRP; i++)
		hsegget();
	chk("fn 59 with an empty pool", bcall(59, (i16)0x5c), B_RUN);
	chk("... refuses with 0FFFFh", bm.r[R_AX] & 0xffff, 0xffff);
	chk("... in BX as well", bm.r[R_BX] & 0xffff, 0xffff);
	chk("... and the pool is as it was", i86nseg, 2);
	for (i = 0; i < CMD_NGRP; i++)
		hsegput(lseg[i]);

	/* A file that is not there is the same refusal. */
	memcpy(&bseg[0x5c + 1], "NOSUCH  CMD", 11);
	chk("fn 59 for a file that is not there", bcall(59, (i16)0x5c),
		B_RUN);
	chk("... refuses with 0FFFFh", bm.r[R_AX] & 0xffff, 0xffff);
	chk("... and took no segment", lsheld(), 0);

	/* Function 49 is answered, not refused.  With no paragraph 0 there
	 * is no system data block, and DDT86 reads 0FFFFh as none. */
	chk("fn 49 answered", bcall(49, (i16)0x20), B_RUN);
	chk("... with 0FFFFh in BX", bm.r[R_BX] & 0xffff, 0xffff);

	/* And the group that is still refused by name. */
	chk("fn 55 still refused", bcall(55, (i16)0), B_FN);
	chk("fn 58 still refused", bcall(58, (i16)0), B_FN);

	i86segget = 0;
	i86segput = 0;
	sysmode = SYS_REC;
}

/*
 * The gate's own command, on the host: copy a file with PIP, then
 * compare.  The input is 1,024 bytes -- a whole number of CP/M records,
 * so a correct copy is byte-identical with no ^Z padding to argue
 * about -- and holds no 0x1A, which PIP would read as end of file.
 */
static void t_pip(const char *dir)
{
	struct ld L;
	struct i86in in;
	char path[512];
	struct sfile *fi, *fo;
	long k, nstep;
	int rc, brc, i, done, bad;

	sprintf(path, "%s/PIP.CMD", dir);
	rc = ldread(path, &L);
	if (rc != CE_OK) {
		printf("i86test: %s: %s -- section 8b skipped\n",
			path, rc < 0 ? "unreadable" : i86cerr(rc));
		ldfree(&L);
		return;
	}
	chk("pip place", ldplace(&L, " VERIFY.OUT=VERIFY.IN"), CE_OK);

	memset(sdisk, 0, sizeof sdisk);
	memset(sfncount, 0, sizeof sfncount);
	sconn = 0;
	sdma = 0;
	fi = &sdisk[0];
	smkname(fi->name, "VERIFY.IN");
	fi->used = 1;
	fi->len = 1024;
	for (k = 0; k < fi->len; k++)
		fi->d[k] = (char)(0x20 + ((k * 7 + (k >> 5)) % 0x5e));

	sysmode = SYS_CPM;
	i86ninsn = i86nflag = 0;
	i86nsegslow = i86nsegbad = 0;
	i86bdosinit(&L.m);
	sysmode = SYS_CPM;

	done = 0;
	brc = B_RUN;
	rc = X_OK;
	for (nstep = 0; nstep < 20000000L; nstep++) {
		rc = i86step(&L.m, &in);
		if (rc == X_OK)
			continue;
		if (rc != X_INT)
			break;
		brc = i86bdos(&L.m);
		if (brc == B_RUN)
			continue;
		done = 1;
		break;
	}
	/* The same call i86.c makes when its loop ends: a run that
	 * stopped anywhere but the seam still owes the console whatever
	 * function 2 had collected. */
	i86oflush();

	/*
	 * K2's number, measured rather than assumed
	 * (CPM86-STAGE-ONE.md §6): "measure the flag-read rate during
	 * step 1, on the host, for free".  This is that measurement.  A
	 * rate near 100 % would mean the lazy scheme has degraded to an
	 * eager one and costs a record keep for nothing.
	 */
	printf("i86test: PIP ran %ld instructions, %lu BDOS calls, "
		"%lu flag materialisations (%ld %% of instructions)\n",
		nstep, (unsigned long)i86nbdos, (unsigned long)i86nflag,
		nstep ? (long)((i86nflag * 100L) / (i32)nstep) : 0L);
	printf("i86test: PIP console: \"");
	for (i = 0; i < sconn; i++) {
		if (scon[i] == '\r')
			continue;
		if (scon[i] == '\n')
			printf("\\n");
		else
			putchar(scon[i]);
	}
	printf("\"\n");
	printf("i86test: PIP BDOS functions used:");
	for (i = 0; i < (int)(sizeof sfncount / sizeof sfncount[0]); i++)
		if (sfncount[i])
			printf(" %d(%ld)", i, sfncount[i]);
	printf("\n");
	if (!done || brc != B_EXIT)
		printf("i86test: PIP stopped: step %s, seam %s (fn %d)\n",
			rc == X_OK ? "ok" :
			rc == X_UNIMP ? "X_UNIMP" :
			rc == X_BAD ? "X_BAD" :
			rc == X_HALT ? "X_HALT" :
			rc == X_SEGESC ? "X_SEGESC" :
			rc == X_WBOOT ? "X_WBOOT" :
			rc == X_WINDOW ? "X_WINDOW" : "X_INT",
			i86berr(), i86bdosfn);

	chk("pip exited cleanly", brc, B_EXIT);
	/* K3, measured on a real program rather than on a static decode:
	 * PIP writes segment registers and every value it writes is one
	 * we handed it, so the slow path never fires and nothing had to
	 * be refused.  The static census predicted this
	 * (CPM86-SHIM-FEASIBILITY.md §1.3); this is it happening. */
	chk("pip no slow segments", (long)i86nsegslow, 0);
	chk("pip no refused segments", (long)i86nsegbad, 0);
	smkname(path, "VERIFY.OUT");
	fo = sfind(path);
	ntest++;
	if (fo == 0) {
		fail("pip made VERIFY.OUT", 0, 1);
	} else {
		chk("pip copy length", fo->len, fi->len);
		bad = 0;
		for (k = 0; k < fi->len && k < fo->len; k++)
			if (fo->d[k] != fi->d[k])
				bad++;
		chk("pip copy identical", bad, 0);
	}
	ldfree(&L);
}


/* ---- 8c: DRI's SUBMIT.CMD, the second real program ---- */

/* SUBMIT verifies the default FCB and functions 25 and 32, then writes the
 * fixed $$$.SUB format: reversed 128-byte command records with a length byte.
 * Its PL/M runtime exits by restoring the entry stack and jumping to offset
 * zero in the entry SS, the CP/M-86 warm-boot convention recognized by wboot(). */
static void t_submit(const char *dir)
{
	struct ld L;
	struct i86in in;
	char path[512];
	struct sfile *fi, *fo;
	FILE *fp;
	long k, nstep;
	int rc, brc, i, done, bad;
	static const char lines[] = "DIR\r\nSTAT\r\n";

	sprintf(path, "%s/SUBMIT.CMD", dir);
	rc = ldread(path, &L);
	if (rc != CE_OK) {
		printf("i86test: %s: %s -- section 8c skipped\n",
			path, rc < 0 ? "unreadable" : i86cerr(rc));
		ldfree(&L);
		return;
	}
	chk("submit place", ldplace(&L, " I86SUB"), CE_OK);

	memset(sdisk, 0, sizeof sdisk);
	memset(sfncount, 0, sizeof sfncount);
	sconn = 0;
	sdma = 0;
	/* One record, ^Z padded, the way a CP/M file with 11 bytes in it
	 * really sits on a disk. */
	fi = &sdisk[0];
	smkname(fi->name, "I86SUB.SUB");
	fi->used = 1;
	fi->len = 128;
	for (k = 0; k < fi->len; k++)
		fi->d[k] = k < (long)(sizeof lines - 1) ? lines[k] : 0x1a;

	sysmode = SYS_CPM;
	i86ninsn = i86nflag = 0;
	i86nsegslow = i86nsegbad = 0;
	i86bdosinit(&L.m);
	sysmode = SYS_CPM;

	done = 0;
	brc = B_RUN;
	rc = X_OK;
	for (nstep = 0; nstep < 20000000L; nstep++) {
		rc = i86step(&L.m, &in);
		if (rc == X_OK)
			continue;
		if (rc != X_INT)
			break;
		brc = i86bdos(&L.m);
		if (brc == B_RUN)
			continue;
		done = 1;
		break;
	}
	/* The same call i86.c makes when its loop ends: a run that
	 * stopped anywhere but the seam still owes the console whatever
	 * function 2 had collected. */
	i86oflush();

	printf("i86test: SUBMIT ran %ld instructions, %lu BDOS calls, "
		"%lu flag materialisations (%ld %% of instructions)\n",
		nstep, (unsigned long)i86nbdos, (unsigned long)i86nflag,
		nstep ? (long)((i86nflag * 100L) / (i32)nstep) : 0L);
	printf("i86test: SUBMIT console: \"");
	for (i = 0; i < sconn; i++) {
		if (scon[i] == '\r')
			continue;
		if (scon[i] == '\n')
			printf("\\n");
		else
			putchar(scon[i]);
	}
	printf("\"\n");
	printf("i86test: SUBMIT BDOS functions used:");
	for (i = 0; i < (int)(sizeof sfncount / sizeof sfncount[0]); i++)
		if (sfncount[i])
			printf(" %d(%ld)", i, sfncount[i]);
	printf("\n");
	printf("i86test: SUBMIT ended at cs:ip %04x:%04x, %s\n",
		(unsigned)L.m.sr[S_CS], (unsigned)L.m.ip,
		rc == X_WBOOT ? "a warm boot" : "somewhere else");

	/* The console must be SILENT: every message SUBMIT can print is
	 * an error, and the one the missing FCB produced was
	 * "Error On Line 001 No 'SUB' File Present". */
	chk("submit said nothing", (long)sconn, 0);
	/* The end, exactly: the far indirect JMP that closes the PL/M-86
	 * epilogue at CS:0034, recognised as this environment's warm boot
	 * with the answer already written.  `done' is still 0 because the
	 * seam never saw a function 0 -- SUBMIT does not call one -- so
	 * the run ends in the EXECUTOR and not in i86bdos(). */
	chk("submit did not exit through the seam", (long)done, 0);
	chk("submit end is X_WBOOT", (long)rc, X_WBOOT);
	chk("submit end is a far indirect jmp", (long)in.op, I_JMPI);
	chk("submit end is the far form", (long)in.x, 1);
	chk("submit end ip", (long)L.m.ip, 0x0054L);
	/* The target it recognised, read back out of the guest's own code
	 * segment: the far pointer at CS:0059 is <entry SS>:0000, and the
	 * entry SS is the paragraph i86place() recorded.  Without both
	 * halves the rule above is an assertion about nothing. */
	chk("submit end target offset",
		(long)((L.m.sb[S_CS][0x59] & 0xff)
		     | ((L.m.sb[S_CS][0x5a] & 0xff) << 8)), 0L);
	chk("submit end target segment",
		(long)((L.m.sb[S_CS][0x5b] & 0xff)
		     | ((L.m.sb[S_CS][0x5c] & 0xff) << 8)),
		(long)(L.m.wseg & 0xffff));
	chk("submit entry ss was recorded", (long)L.m.wset, 1);
	chk("submit no slow segments", (long)i86nsegslow, 0);
	chk("submit no refused segments", (long)i86nsegbad, 0);

	/* It read the file the DEFAULT FCB named, which is the whole
	 * point of picking this binary. */
	chk("submit opened the .SUB", sfncount[15], 1);
	chk("submit asked the current disk", sfncount[25], 1);
	chk("submit asked the user code", sfncount[32], 1);
	chk("submit closed its output", sfncount[16], 1);

	smkname(path, "$$$.SUB");
	fo = sfind(path);
	ntest++;
	if (fo == 0) {
		fail("submit made $$$.SUB", 0, 1);
	} else {
		/* Two command lines, two records, REVERSED: record 0 is
		 * the last line of the file.  Byte 0 of each record is
		 * the length; the text follows. */
		chk("submit $$$.SUB length", fo->len, 256L);
		chk("submit record 0 count", (long)(fo->d[0] & 0xff), 4);
		bad = memcmp(fo->d + 1, "STAT", 4) != 0;
		chk("submit record 0 text", (long)bad, 0);
		chk("submit record 1 count", (long)(fo->d[128] & 0xff), 3);
		bad = memcmp(fo->d + 129, "DIR", 3) != 0;
		chk("submit record 1 text", (long)bad, 0);
		/* The bytes themselves, for tests/verify.mk to compare the
		 * target's copy against.  The tail of each record is
		 * whatever was in SUBMIT's own buffer, so a byte-for-byte
		 * match between the two runs says the guest's data group
		 * was zeroed the same way in both -- which is more than a
		 * transcribed fixture could ever say.  A failure to write
		 * it is not a test failure: the host run stands on its own
		 * and this is a by-product for another target. */
		fp = fopen("build/i86sub-host.bin", "wb");
		if (fp) {
			fwrite(fo->d, 1, (size_t)fo->len, fp);
			fclose(fp);
		}
	}
	ldfree(&L);
}

/* ---- 8d: DRI's GENCMD.CMD, and the round trip through our own format ---- */

/*
 * WHY GENCMD, GIVEN THAT SECTION 8c JUST SAID IT GRADES ITSELF.
 *
 * It does, and that is why it is here LAST and why nothing above it
 * depends on it.  What it buys is the one thing PIP and SUBMIT cannot
 * say: those two are handed a command line and answer with a file whose
 * format is somebody else's.  GENCMD is handed a FILE -- Intel hex, in
 * DRI's dialect -- and answers with a .CMD, which means:
 *
 *   - it is the only corpus binary that exercises the loader's input
 *     format from the far end.  A header this shim wrote for itself
 *     would prove nothing; a header DRI's own GENCMD wrote, out of hex
 *     WE supplied, and that our loader then reads and RUNS, closes the
 *     loop through code neither end wrote.
 *   - it is the binary that reads the base page's data group size and
 *     ACTS on it, comparing the top of its own buffer against the word
 *     at DS:6 before every byte it stores (GENCMD.CMD 08A0h).  PIP and
 *     SUBMIT read the size and buffer against it; GENCMD reads it,
 *     decides it is too small, prints "INSUFFICIENT MEMORY TO CREATE CMD
 *     FILE" and quits after 790 instructions.  It is the reason galloc()
 *     had to grow a group toward G-Max, and the reason the group table's
 *     length is a byte count -- see i86bpage().
 *
 * Its input is build/cmdfix/I86HEX.H86, built by tools/mkcmdfix.py
 * p_hex(): a real hex file with real checksums around eleven bytes of
 * hand-assembled 8086 that prints "I86HEX OK" through INT 0E0h and
 * exits.  The grading is NOT "the header we read back matches the header
 * we expected" -- that is the self-grading trap section 8c names.  It
 * is: load the .CMD GENCMD wrote, run it, and see the string come out of
 * the console.  A wrong group descriptor, a wrong image offset or a
 * wrong entry point all fail that, and none of them can be papered over
 * by our own reader agreeing with our own writer.
 */
static void t_gencmd(const char *dir, const char *fixdir)
{
	struct ld L;
	struct i86in in;
	char path[512];
	struct sfile *fi, *fo;
	FILE *fp;
	long nstep, hlen, clen;
	int rc, brc, i, done;
	static char hex[SF_CAP];
	static char cmd[SF_CAP];

	sprintf(path, "%s/I86HEX.H86", fixdir);
	fp = fopen(path, "rb");
	if (fp == 0) {
		printf("i86test: %s: unreadable -- section 8d skipped\n", path);
		return;
	}
	hlen = (long)fread(hex, 1, sizeof hex, fp);
	fclose(fp);

	sprintf(path, "%s/GENCMD.CMD", dir);
	rc = ldread(path, &L);
	if (rc != CE_OK) {
		printf("i86test: %s: %s -- section 8d skipped\n",
			path, rc < 0 ? "unreadable" : i86cerr(rc));
		ldfree(&L);
		return;
	}
	chk("gencmd place", ldplace(&L, " I86HEX"), CE_OK);

	memset(sdisk, 0, sizeof sdisk);
	memset(sfncount, 0, sizeof sfncount);
	sconn = 0;
	sdma = 0;
	fi = &sdisk[0];
	smkname(fi->name, "I86HEX.H86");
	fi->used = 1;
	fi->len = hlen;
	memcpy(fi->d, hex, (size_t)hlen);

	sysmode = SYS_CPM;
	i86ninsn = i86nflag = 0;
	i86nsegslow = i86nsegbad = 0;
	i86bdosinit(&L.m);
	sysmode = SYS_CPM;

	done = 0;
	brc = B_RUN;
	rc = X_OK;
	for (nstep = 0; nstep < 20000000L; nstep++) {
		rc = i86step(&L.m, &in);
		if (rc == X_OK)
			continue;
		if (rc != X_INT)
			break;
		brc = i86bdos(&L.m);
		if (brc == B_RUN)
			continue;
		done = 1;
		break;
	}
	/* The same call i86.c makes when its loop ends: a run that
	 * stopped anywhere but the seam still owes the console whatever
	 * function 2 had collected. */
	i86oflush();
	printf("i86test: GENCMD ran %ld instructions, %lu BDOS calls, "
		"%lu flag materialisations (%ld %% of instructions)\n",
		nstep, (unsigned long)i86nbdos, (unsigned long)i86nflag,
		nstep ? (long)((i86nflag * 100L) / (i32)nstep) : 0L);
	printf("i86test: GENCMD console: \"");
	for (i = 0; i < sconn; i++) {
		if (scon[i] == '\r')
			continue;
		if (scon[i] == '\n')
			printf("\\n");
		else
			putchar(scon[i]);
	}
	printf("\"\n");
	printf("i86test: GENCMD BDOS functions used:");
	for (i = 0; i < (int)(sizeof sfncount / sizeof sfncount[0]); i++)
		if (sfncount[i])
			printf(" %d(%ld)", i, sfncount[i]);
	printf("\n");
	printf("i86test: GENCMD stopped at cs:ip %04x:%04x, done %d, "
		"step rc %d, seam %s\n",
		(unsigned)L.m.sr[S_CS], (unsigned)L.m.ip, done, rc, i86berr());

	/* It exited through BDOS function 0 rather than stopping on
	 * something we do not implement -- GENCMD is the only one of the
	 * four corpus binaries that runs to completion. */
	chk("gencmd exited cleanly", brc, B_EXIT);
	chk("gencmd instructions", nstep, 1911524L);
	chk("gencmd BDOS calls", (long)i86nbdos, 38L);
	chk("gencmd no slow segments", (long)i86nsegslow, 0);
	chk("gencmd no refused segments", (long)i86nsegbad, 0);
	/* Its own report of what it did, which is also the check that no
	 * error message got in: every other thing GENCMD prints is one.
	 * 001B bytes is the 27 the three hex records carry, and four
	 * 128-byte records is the 512-byte .CMD below. */
	scon[sconn < (int)sizeof scon ? sconn : (int)sizeof scon - 1] = 0;
	ntest++;
	if (strstr(scon, "BYTES READ    001B") == 0
	    || strstr(scon, "RECORDS WRITTEN 04") == 0)
		fail("gencmd reported the conversion", 1, 0);
	/* It read the .H86 the DEFAULT FCB named, without being told the
	 * extension: GENCMD appends "H86" itself. */
	chk("gencmd opened the .H86", sfncount[15], 4);

	smkname(path, "I86HEX.CMD");
	fo = sfind(path);
	ntest++;
	if (fo == 0) {
		fail("gencmd made I86HEX.CMD", 0, 1);
		ldfree(&L);
		return;
	}
	chk("gencmd .CMD length", fo->len, 512L);
	clen = fo->len;
	memcpy(cmd, fo->d, (size_t)clen);
	printf("i86test: GENCMD wrote %ld bytes of .CMD\n", clen);
	/* For tests/verify.mk to `cmp' the target's copy against, the way
	 * build/i86sub-host.bin serves section 8c.  Not a test failure if
	 * it cannot be written: the host run stands on its own. */
	fp = fopen("build/i86hex-host.cmd", "wb");
	if (fp) {
		fwrite(cmd, 1, (size_t)clen, fp);
		fclose(fp);
	}
	ldfree(&L);

	/*
	 * THE ROUND TRIP.  Everything above could be true of a GENCMD that
	 * wrote a plausible header full of wrong numbers; the only reader
	 * that would catch it is one that does not share our assumptions,
	 * and there is no such reader here.  So do not read the header --
	 * RUN it.  The program was written in tools/mkcmdfix.py p_hex() to
	 * print one string and exit, and the string coming out of the
	 * console is the statement that GENCMD's group descriptors, image
	 * offsets and entry point and our loader's reading of them agree.
	 */
	fp = fopen("build/i86hex-host.cmd", "rb");
	if (fp == 0) {
		printf("i86test: build/i86hex-host.cmd: unwritable -- "
			"round trip skipped\n");
		return;
	}
	fclose(fp);
	rc = ldread("build/i86hex-host.cmd", &L);
	chk("gencmd output loads", rc, CE_OK);
	if (rc != CE_OK) {
		ldfree(&L);
		return;
	}
	/* Small model, because the hex named a data segment as well as a
	 * code one -- read back off GENCMD's header, not asserted from
	 * our own writer's intent. */
	chk("gencmd output model", L.c.model, M_SMALL);
	chk("gencmd output ng", L.c.ng, 2);
	chk("gencmd output entry", L.c.entry, 0);
	chk("gencmd output code G-Length", L.c.g[0].len, 1);
	chk("gencmd output data G-Length", L.c.g[1].len, 17);
	chk("gencmd output place", ldplace(&L, ""), CE_OK);

	memset(sdisk, 0, sizeof sdisk);
	memset(sfncount, 0, sizeof sfncount);
	sconn = 0;
	sdma = 0;
	sysmode = SYS_CPM;
	i86bdosinit(&L.m);
	sysmode = SYS_CPM;
	done = 0;
	brc = B_RUN;
	rc = X_OK;
	for (nstep = 0; nstep < 1000L; nstep++) {
		rc = i86step(&L.m, &in);
		if (rc == X_OK)
			continue;
		if (rc != X_INT)
			break;
		brc = i86bdos(&L.m);
		if (brc == B_RUN)
			continue;
		done = 1;
		break;
	}
	/* The same call i86.c makes when its loop ends: a run that
	 * stopped anywhere but the seam still owes the console whatever
	 * function 2 had collected. */
	i86oflush();
	scon[sconn < (int)sizeof scon ? sconn : (int)sizeof scon - 1] = 0;
	printf("i86test: round trip ran %ld instructions, console \"%s\"\n",
		nstep, scon);
	chk("round trip exited", brc, B_EXIT);
	chk("round trip steps", nstep, 4L);
	ntest++;
	if (strcmp(scon, "I86HEX OK\r\n") != 0)
		fail("round trip printed I86HEX OK", 1, 0);
	ldfree(&L);
}

/* ---- 8h: ASM86, STAT, HELP and TOD ---- */

/* Put a text file on the stub disk, ^Z padded to a whole record. */
static struct sfile *sput(const char *name, const char *s, long n)
{
	struct sfile *f;
	char fn[11];
	long k;

	smkname(fn, name);
	f = smake(fn);
	if (f == 0)
		return (0);
	memcpy(f->d, s, (size_t)n);
	f->len = (n + 127) / 128 * 128;
	for (k = n; k < f->len; k++)
		f->d[k] = 0x1a;
	return (f);
}

static struct sfile *sget(const char *name)
{
	char fn[11];

	smkname(fn, name);
	return (sfind(fn));
}

/* Is `s' in the first `n' bytes of `d'? */
static int shas(const char *d, long n, const char *s)
{
	long k, m;

	m = (long)strlen(s);
	for (k = 0; k + m <= n; k++)
		if (memcmp(d + k, s, (size_t)m) == 0)
			return (1);
	return (0);
}

/*
 * Run `path' against the stub disk as it stands, with `keys' as console
 * input.  Returns the B_ code the run ended on, or -1 if it did not load;
 * the console is left NUL-terminated in scon[].
 */
static int crun(const char *path, const char *tail, const char *keys,
	long *nstep)
{
	struct ld L;
	struct i86in in;
	int rc, brc;

	rc = ldread(path, &L);
	if (rc != CE_OK) {
		printf("i86test: %s: %s\n", path,
			rc < 0 ? "unreadable" : i86cerr(rc));
		ldfree(&L);
		return (-1);
	}
	if (ldplace(&L, tail) != CE_OK) {
		ldfree(&L);
		return (-1);
	}
	memset(sfncount, 0, sizeof sfncount);
	sconn = 0;
	sdma = 0;
	skeys = keys;
	skeyeof = 0;
	sysmode = SYS_CPM;
	i86bdosinit(&L.m);
	sysmode = SYS_CPM;
	brc = -2;
	for (*nstep = 0; *nstep < 20000000L; (*nstep)++) {
		rc = i86step(&L.m, &in);
		if (rc == X_OK)
			continue;
		if (rc != X_INT)
			break;
		brc = i86bdos(&L.m);
		if (brc != B_RUN)
			break;
	}
	i86oflush();
	skeys = 0;
	scon[sconn < (int)sizeof scon ? sconn : (int)sizeof scon - 1] = 0;
	printf("i86test: %s%s: %ld instructions, console \"", path, tail,
		*nstep);
	for (rc = 0; rc < sconn; rc++)
		if (scon[rc] == '\n')
			printf("\\n");
		else if (scon[rc] != '\r')
			putchar(scon[rc]);
	printf("\"\n");
	ldfree(&L);
	return (brc);
}

/* Copy a stub file out for tests/verify.mk to compare the machine's with. */
static int sdump(struct sfile *f, const char *path)
{
	FILE *fp;

	if (f == 0 || (fp = fopen(path, "wb")) == 0)
		return (0);
	fwrite(f->d, 1, (size_t)f->len, fp);
	fclose(fp);
	return (1);
}

/*
 * ASM86 assembles I86T.A86 (tools/mkcmdfix.py p_asm()), GENCMD turns the
 * hex into a .CMD, and the .CMD runs.  The code bytes checked in the hex
 * are the 8086 encodings of the six source lines, worked by hand.
 */
static void t_asm86(const char *dir, const char *fixdir)
{
	char path[512];
	struct sfile *f;
	FILE *fp;
	long nstep, n;
	int brc;

	memset(sdisk, 0, sizeof sdisk);
	sprintf(path, "%s/I86T.A86", fixdir);
	fp = fopen(path, "rb");
	if (fp == 0) {
		fail("asm86 source fixture", 0, 1);
		return;
	}
	f = sput("I86T.A86", "", 0L);
	f->len = (long)fread(f->d, 1, sizeof f->d, fp);
	fclose(fp);

	sprintf(path, "%s/ASM86.CMD", dir);
	brc = crun(path, " I86T", 0, &nstep);
	chk("asm86 exited", brc, B_EXIT);
	ntest++;
	if (strstr(scon, "END OF ASSEMBLY.  NUMBER OF ERRORS:   0.") == 0)
		fail("asm86 reported no errors", 1, 0);

	f = sget("I86T.H86");
	n = f ? f->len : 0;
	ntest++;
	if (f == 0 || !shas(f->d, n, ":0D000081B109BA0001CDE0B100B200CDE040")
	 || !shas(f->d, n,
		":1101008248454C4C4F2046524F4D2041534D38362411")
	 || !shas(f->d, n, ":00000001FF"))
		fail("asm86 hex holds the code and data records", 1, 0);
	sdump(f, "build/i86asm-host.h86");
	f = sget("I86T.LST");
	n = f ? f->len : 0;
	ntest++;
	if (f == 0 || !shas(f->d, n, " 0002 BA0001")
	 || !shas(f->d, n, " 0100 48454C4C4F20      MSG"))
		fail("asm86 listing", 1, 0);
	f = sget("I86T.SYM");
	ntest++;
	if (f == 0 || !shas(f->d, f->len, "0100 MSG"))
		fail("asm86 symbol file", 1, 0);

	sprintf(path, "%s/GENCMD.CMD", dir);
	brc = crun(path, " I86T", 0, &nstep);
	chk("asm86 gencmd exited", brc, B_EXIT);
	f = sget("I86T.CMD");
	ntest++;
	if (!sdump(f, "build/i86asm-host.cmd")) {
		fail("gencmd made I86T.CMD", 0, 1);
		return;
	}
	memset(sdisk, 0, sizeof sdisk);
	brc = crun("build/i86asm-host.cmd", "", 0, &nstep);
	chk("I86T.CMD exited", brc, B_EXIT);
	ntest++;
	if (strcmp(scon, "HELLO FROM ASM86") != 0)
		fail("I86T.CMD printed its string", 1, 0);
}

/* STAT sizes files from their directory entries: 1 and 40 records, one
 * and two 4 KB blocks. */
static void t_stat(const char *dir)
{
	static char two[5000];
	char path[512];
	long nstep;

	memset(sdisk, 0, sizeof sdisk);
	sput("ONE.TXT", "x", 1L);
	sput("TWO.DAT", two, (long)sizeof two);
	sprintf(path, "%s/STAT.CMD", dir);
	chk("stat exited", crun(path, "", 0, &nstep), B_EXIT);
	ntest++;
	if (strstr(scon, "A: RW, Free Space:    10,224k") == 0)
		fail("stat free space", 1, 0);
	chk("stat *.* exited", crun(path, " *.*", 0, &nstep), B_EXIT);
	ntest++;
	if (strstr(scon, "    1     4k    1 Dir RW        A:ONE     .TXT") == 0
	 || strstr(scon, "   40     8k    1 Dir RW        A:TWO     .DAT") == 0
	 || strstr(scon, "Total:   12k    2") == 0)
		fail("stat *.* sizes", 1, 0);
	crun(path, " TWO.DAT", 0, &nstep);
	ntest++;
	if (strstr(scon, "A:ONE") != 0 || strstr(scon, "Total:    8k    1") == 0)
		fail("stat one file", 1, 0);
}

static void t_help(const char *dir)
{
	char path[512];
	FILE *fp;
	struct sfile *f;
	long nstep;

	memset(sdisk, 0, sizeof sdisk);
	sprintf(path, "%s/HELP.HLP", dir);
	fp = fopen(path, "rb");
	if (fp == 0) {
		fail("HELP.HLP readable", 0, 1);
		return;
	}
	f = sput("HELP.HLP", "", 0L);
	f->len = (long)fread(f->d, 1, sizeof f->d, fp);
	fclose(fp);
	sprintf(path, "%s/HELP.CMD", dir);
	/* The topic lives at record 114, reached by a random read; the
	 * wrong record shows ASM86's text instead. */
	chk("help exited", crun(path, " PIP", "", &nstep), B_EXIT);
	ntest++;
	if (strstr(scon, "PIP filespec{[Gn]}=filespec{[O]}") == 0
	 || strstr(scon, "Copies files, combines  files") == 0
	 || strstr(scon, "hex file drive") != 0)
		fail("help shows the PIP topic", 1, 0);
}

/*
 * TOD reads and writes the clock string in function 49's block.  Day
 * 17797 is 22 September 2026; 03/01/84 is day 2252.  Setting drops the
 * seconds, as our function 104 does.
 */
static void t_tod(const char *dir)
{
	char path[512];
	struct ld L;
	long nstep, ninsn, nbad, nunimp;

	/* The corpus's one 8080-model file: a single group, measured. */
	sprintf(path, "%s/TOD.CMD", dir);
	chk("TOD.CMD rc", ldread(path, &L), CE_OK);
	chk("TOD.CMD file length", L.flen, 2688);
	chk("TOD.CMD model", L.c.model, M_8080);
	chk("TOD.CMD ng", L.c.ng, 1);
	chk("TOD.CMD code G-Length", L.c.g[0].len, 154);
	chk("TOD.CMD place", ldplace(&L, ""), CE_OK);
	ldsweep(&L, &ninsn, &nbad, &nunimp);
	chk("TOD.CMD instructions", ninsn, 957);
	chk("TOD.CMD undecodable bytes", nbad, 0);
	chk("TOD.CMD refused classes", nunimp, 3);
	ldfree(&L);

	memset(sdisk, 0, sizeof sdisk);
	sclk[0] = (char)0x85; sclk[1] = 0x45;
	sclk[2] = 0x12; sclk[3] = 0x34; sclk[4] = 0x56;
	chk("tod exited", crun(path, "", 0, &nstep), B_EXIT);
	ntest++;
	if (strstr(scon, "09/22/26       12:34:56") == 0)
		fail("tod read the clock", 1, 0);
	chk("tod console width", zseg[0x440], 80);

	chk("tod set exited", crun(path, " 03/01/84 07:08:09", "x", &nstep),
		B_EXIT);
	chk("tod set day", (sclk[0] & 0xff) | (sclk[1] & 0xff) << 8, 2252);
	chk("tod set hour", sclk[2], 0x07);
	chk("tod set minute", sclk[3], 0x08);
	ntest++;
	if (strstr(scon, "Strike key to set time") == 0
	 || strstr(scon, "03/01/84       07:08:00") == 0)
		fail("tod set shows the new time", 1, 0);

	/* TOD's own check: its calendar starts in 1978. */
	crun(path, " 01/02/03 04:05:06", "x", &nstep);
	ntest++;
	if (strstr(scon, "Invalid Date & Time Format") == 0)
		fail("tod refused 2003", 1, 0);
	chk("... and left the clock", sclk[0] & 0xff, 0xcc);
}

/* GSX-86 (i86gsx.c) with no device: GDOS's refusal. */
static void t_gsx86(const char *dir)
{
	char path[512];
	long nstep;

	sprintf(path, "%s/GX86.CMD", dir);
	i86gsxdev = 0;
	memset(scon, 0, sizeof scon);
	chk("gsx86 without a device exits", crun(path, "", 0, &nstep), B_EXIT);
	ntest++;
	if (strstr(scon, " Invalid Gios0000h (hex)\n\rGSX Removed\r\n") == 0
	 || strncmp(scon, "GSX-86 ", 7) != 0)
		fail("gsx86 without a device is GDOS's refusal", 1, 0);

}

/* ==================================================================
 * THE DMA WINDOW A MULTI-SECTOR TRANSFER ACTUALLY USES.
 *
 * i86bdos.c's setdma() validated I86DMA -- 128 bytes, ONE record --
 * while BDOS function 44 was an ordinary P_BYTE that handed the guest's
 * record count straight to the native BDOS, whose multio()
 * (src/bdos/bdosrw.c:342) loops that many times adding SECLEN to the DMA
 * address between records.  So a guest that said "two records" and put
 * its DMA offset at 0xff80 -- accepted, because one record ends exactly
 * at the top of the 64 KB segment -- had 256 bytes written from 0xff80
 * and the second record landed outside the segment.
 *
 * THE RETURN CODE IS NOT THE OBJECT: what matters is whether anything
 * above the segment was written.  So the guest's data segment here is
 * the front of a larger array whose tail holds a canary, and the check
 * is on the canary.  Under -fsanitize=address the same write against
 * bseg[] aborts the run too (tests/verify.mk verify-shim); this check
 * does not need that build to see it.
 */

#define CAN	0x5a			/* the canary byte		*/
#define CANN	128			/* how much of it there is	*/

static char dseg2[65536 + CANN];	/* the guest's DS, plus a canary */

static int canary(void)			/* first byte written past 64 KB */
{
	int i;

	for (i = 0; i < CANN; i++)
		if ((dseg2[65536 + i] & 0xff) != CAN)
			return (dseg2[65536 + i] & 0xff);
	return (-1);
}

/* bsetup(), with a data segment that has a canary behind it. */
static void bsetup2(void)
{
	memset(dseg2, 0, 65536);
	memset(dseg2 + 65536, CAN, CANN);
	memset(cseg, 0, sizeof cseg);
	memset(&bm, 0, sizeof bm);
	bm.sb[S_CS] = cseg;
	bm.sr[S_CS] = 0x1000;
	bm.sb[S_DS] = bm.sb[S_SS] = bm.sb[S_ES] = dseg2;
	bm.sr[S_DS] = bm.sr[S_SS] = bm.sr[S_ES] = 0x2000;
	bm.lz = LZ_NONE;
	bm.fl = F_ONES;
	bm.ip = 0;
	bm.r[R_SP] = 0xff00;
	i86nseg = 2;
	i86spar[0] = 0x1000; i86sbase[0] = cseg;
	i86spar[1] = 0x2000; i86sbase[1] = dseg2;
	sysmode = SYS_CPM;
	sncall = 0;
	slast_fn = -1;
	smultcnt = 1;
	sdma = 0;
	memset(sbase, 0, sizeof sbase);
	memset(sdisk, 0, sizeof sdisk);
	memset(sfncount, 0, sizeof sfncount);
	sconn = 0;
	ssearch = 0;
	i86bdosinit(&bm);
}

static void t_dmabound(void)
{
	struct sfile	*f;
	long		k;
	int		rc;

	bsetup2();

	f = &sdisk[0];
	smkname(f->name, "MULTI.DAT");
	f->used = 1;
	f->len = 8L * 128L;
	for (k = 0; k < f->len; k++)
		f->d[k] = (char)(0x40 + (int)(k / 128));  /* record N = '@'+N */

	memset(dseg2 + 0x0100, 0, 36);
	smkname(dseg2 + 0x0100 + 1, "MULTI.DAT");
	chk("fn 15 opens the multi-sector file", bcall(15, 0x0100), B_RUN);

	/* ---- ONE record ending exactly at the top of the segment is
	   legal, and the bound must leave it legal. */

	chk("fn 26 accepts a DMA whose one record ends at 0x10000",
		bcall(26, (i16)0xff80), B_RUN);
	chk("fn 20 reads one record into the top of the segment",
		bcall(20, 0x0100), B_RUN);
	chk("... record 0 is there", (long)(dseg2[0xff80] & 0xff), 0x40L);
	chk("... its last byte is the segment's last byte",
		(long)(dseg2[0xffff] & 0xff), 0x40L);
	chk("... with nothing written above it", (long)canary(), -1L);

	/* ---- TWO records from that same DMA.  THE OBJECT: the second
	   record must not appear above the segment. */

	chk("fn 44 accepts a count of two", bcall(44, 2), B_RUN);
	rc = bcall(20, 0x0100);
	chk("a 2-record fn 20 from 0xff80 writes NOTHING above the guest "
	    "segment", (long)canary(), -1L);
	chk("... and is refused as an address error", rc, B_ADDR);
	chk("... without reaching the native BDOS a second time",
		sfncount[20], 1L);
	chk("a 2-record fn 21 from 0xff80 is refused",
		bcall(21, 0x0100), B_ADDR);
	chk("... and wrote nothing above the segment", (long)canary(), -1L);

	/* ---- a count of two with room for two still works, which is
	   the half of this that a blunter fix would have broken. */

	chk("fn 26 moves the DMA down", bcall(26, 0x2000), B_RUN);
	chk("a 2-record fn 20 with room runs", bcall(20, 0x0100), B_RUN);
	chk("... record 1 first", (long)(dseg2[0x2000] & 0xff), 0x41L);
	chk("... record 2 next", (long)(dseg2[0x2000 + 128] & 0xff), 0x42L);
	chk("... in one guest-visible BDOS call", sfncount[20], 2L);

	/* ---- eight records ending exactly at the top: still legal. */

	chk("fn 44 accepts a count of eight", bcall(44, 8), B_RUN);
	chk("fn 26 accepts the DMA eight records fit in",
		bcall(26, (i16)(0x10000L - 8 * 128)), B_RUN);
	memset(dseg2 + 0x0100, 0, 36);		/* a fresh FCB, record 0 */
	smkname(dseg2 + 0x0100 + 1, "MULTI.DAT");
	chk("fn 15 reopens it", bcall(15, 0x0100), B_RUN);
	chk("an 8-record fn 20 ending at 0x10000 runs",
		bcall(20, 0x0100), B_RUN);
	chk("... record 0 at the bottom of the window",
		(long)(dseg2[0x10000L - 8 * 128] & 0xff), 0x40L);
	chk("... record 7 at the top", (long)(dseg2[0xff80] & 0xff), 0x47L);
	chk("... and nothing above it", (long)canary(), -1L);

	/* ---- one more record than fits, by one record. */

	memset(dseg2 + 0x0100, 0, 36);
	smkname(dseg2 + 0x0100 + 1, "MULTI.DAT");
	bcall(15, 0x0100);
	chk("fn 44 accepts a count of nine", bcall(44, 9), B_RUN);
	chk("a 9-record transfer into an 8-record window is refused",
		bcall(20, 0x0100), B_ADDR);
	chk("... having written nothing above the segment",
		(long)canary(), -1L);

	/* ---- and a REFUSED function 26 or 51 must leave the shim
	   holding the DMA the native BDOS was actually told. */

	chk("fn 44 back to one record", bcall(44, 1), B_RUN);
	chk("fn 26 accepts 0x3000", bcall(26, 0x3000), B_RUN);
	chk("a DMA offset 127 from the top is refused",
		bcall(26, (i16)(0x10000L - 127)), B_ADDR);
	chk("... and i86dmaoff still holds the accepted one",
		(long)(i86dmaoff & 0xffff), 0x3000L);
	chk("a DMA base we never handed out is refused",
		bcall(51, 0x7000), B_SEG);
	chk("... and i86dmaseg still holds the accepted one",
		(long)(i86dmaseg & 0xffff), 0x2000L);
	chk("... so the next transfer still works",
		bcall(20, 0x0100), B_RUN);
}

/* ==================================================================
 * RANDOM RECORD I/O -- functions 33, 34 and 40, through the seam.
 *
 * These three are the FCB calls whose position comes out of the FCB
 * itself rather than out of a sequential cursor, so everything that can
 * go wrong with them goes wrong in the FCB: the record number's byte
 * order, and whether multio() puts the caller's copy of it back.  No
 * guest program is needed to reach them -- a read or write is fully
 * described by an FCB and a DMA address, both of which this test can
 * place in dseg2[] itself.
 *
 * i86bdos.c's ranswap() sits between every call here and the stub: the
 * FCB is built and read back in the GUEST's little-endian order (r0 at
 * +33 is the low byte), and it is the seam, not this test, that flips
 * it to the order srrec()/srincr() use.  A byte-order mistake on either
 * side would show up here as the wrong record read back, which is the
 * class of bug the Z80 lane found as DUMP's "No Records Exist"
 * (Z80-STAGE-ONE.md §0.2).
 */
static void t_random(void)
{
	struct sfile *f;
	long k;

	bsetup2();

	f = &sdisk[0];
	smkname(f->name, "RANDOM.DAT");
	f->used = 1;
	f->len = 4L * 128L;
	for (k = 0; k < f->len; k++)
		f->d[k] = (char)(k / 128);	/* record N is N in every byte */

	/* byte 0 of a CP/M FCB is the drive, the name starts at byte 1 --
	 * sfind(addr + 1) in the stub is its own reminder of that. */
	memset(dseg2 + 0x0100, 0, 36);
	smkname(dseg2 + 0x0100 + 1, "RANDOM.DAT");

	chk("fn 26 sets the DMA the random tests use",
		bcall(26, 0x2000), B_RUN);

	/* ---- read random, three records in one multi-sector call ---- */

	chk("fn 44 accepts a multi-sector count", bcall(44, 3), B_RUN);

	dseg2[0x0100 + 33] = 1;			/* record 1, guest order:  */
	dseg2[0x0100 + 34] = 0;			/* r0 (low) = 1, r1 = r2 = 0 */
	dseg2[0x0100 + 35] = 0;
	chk("fn 33 multi-sector random read runs", bcall(33, 0x0100), B_RUN);
	chk("... in one guest-visible BDOS call", sfncount[33], 1L);
	chk("... record 1 first", (long)(dseg2[0x2000] & 0xff), 1L);
	chk("... record 2 next", (long)(dseg2[0x2000 + 128] & 0xff), 2L);
	chk("... record 3 last", (long)(dseg2[0x2000 + 256] & 0xff), 3L);
	/* multio() restores the caller's random-record field exactly;
	 * these three bytes are still in the GUEST's order because
	 * i86bdos.c un-swaps them again before returning. */
	chk("fn 33 restores the guest's r0",
		(long)(dseg2[0x0100 + 33] & 0xff), 1L);
	chk("fn 33 restores the guest's r1",
		(long)(dseg2[0x0100 + 34] & 0xff), 0L);
	chk("fn 33 restores the guest's r2",
		(long)(dseg2[0x0100 + 35] & 0xff), 0L);

	/* ---- write random, single record, well within the file ---- */

	chk("fn 44 back to one record per call", bcall(44, 1), B_RUN);

	memset(dseg2 + 0x2000, (char)0xbb, 128);
	dseg2[0x0100 + 33] = 2;			/* record 2		*/
	dseg2[0x0100 + 34] = 0;
	dseg2[0x0100 + 35] = 0;
	chk("fn 34 random write runs", bcall(34, 0x0100), B_RUN);
	chk("... counted", sfncount[34], 1L);
	chk("... record 2 now holds the new pattern",
		(long)(f->d[2 * 128] & 0xff), 0xbbL);
	chk("... record 1 is untouched",
		(long)(f->d[1 * 128] & 0xff), 1L);

	/* ---- write random with zero fill, two records past EOF ---- */

	chk("fn 44 accepts a count of two", bcall(44, 2), B_RUN);

	memset(dseg2 + 0x2000, (char)0xcc, 128);
	memset(dseg2 + 0x2000 + 128, (char)0xdd, 128);
	dseg2[0x0100 + 33] = 10;	/* record 10, six past the	*/
	dseg2[0x0100 + 34] = 0;		/* 4-record file's old EOF	*/
	dseg2[0x0100 + 35] = 0;
	chk("fn 40 write-random-with-zero-fill runs",
		bcall(40, 0x0100), B_RUN);
	chk("... in one guest-visible BDOS call", sfncount[40], 1L);
	chk("... record 10 holds the first write",
		(long)(f->d[10 * 128] & 0xff), 0xccL);
	chk("... record 11 holds the second write",
		(long)(f->d[11 * 128] & 0xff), 0xddL);
	/* the gap between the old 4-record EOF and record 10 reads back
	 * zero, not leftover disk content -- the promise fn 40 makes */
	chk("... the gap (record 5) is zero-filled",
		(long)(f->d[5 * 128] & 0xff), 0L);
	chk("... the gap (record 9) is zero-filled",
		(long)(f->d[9 * 128] & 0xff), 0L);

	/* ---- a random read of a hole is still an error ---- */

	chk("fn 44 back to one record", bcall(44, 1), B_RUN);
	dseg2[0x0100 + 33] = 99;
	dseg2[0x0100 + 34] = 0;
	dseg2[0x0100 + 35] = 0;
	bcall(33, 0x0100);
	chk("fn 33 past EOF answers error 1",
		(long)(bm.r[R_AX] & 0xff), 1L);
	chk("... and BX carries the same word",
		(long)(bm.r[R_BX] & 0xff), 1L);

	/* ---- a record number past the largest file CP/M can name is a
	   DIFFERENT answer: code 6, refused before the file is looked at
	   (src/bdos/bdosrw.c new_ext, `mod >= 64'). */

	dseg2[0x0100 + 33] = 0;		/* record 0x040000, guest order */
	dseg2[0x0100 + 34] = 0;
	dseg2[0x0100 + 35] = 4;
	bcall(33, 0x0100);
	chk("fn 33 past the maximum file size answers error 6",
		(long)(bm.r[R_AX] & 0xff), 6L);
	bcall(34, 0x0100);
	chk("fn 34 past the maximum file size answers error 6",
		(long)(bm.r[R_AX] & 0xff), 6L);

	chk("random i/o wrote nothing above the guest segment",
		(long)canary(), -1L);

	/* ---- the DMA bound covers the random three as well: a
	   multi-record transfer that would leave the segment is refused
	   before the native BDOS is told. */

	chk("fn 44 accepts a count of two again", bcall(44, 2), B_RUN);
	chk("fn 26 accepts a DMA one record from the top",
		bcall(26, (i16)0xff80), B_RUN);
	dseg2[0x0100 + 33] = 0;
	dseg2[0x0100 + 34] = 0;
	dseg2[0x0100 + 35] = 0;
	chk("a 2-record fn 33 from 0xff80 is refused",
		bcall(33, 0x0100), B_ADDR);
	chk("a 2-record fn 34 from 0xff80 is refused",
		bcall(34, 0x0100), B_ADDR);
	chk("a 2-record fn 40 from 0xff80 is refused",
		bcall(40, 0x0100), B_ADDR);
	chk("... having written nothing above the segment",
		(long)canary(), -1L);
	chk("... and without reaching the native BDOS", sfncount[33], 3L);
}

/* ==================================================================
 * THE PREFIX LOOP HAS A BOUND.
 *
 * i86dec()'s prefix loop was `for (;;)' with no limit, and its fetch
 * wraps at 16 bits by design (i86dec.c fb()), so a code segment filled
 * with 0x26 never left the loop -- the hang was INSIDE one i86dec()
 * call, where the executor's own step limit never gets a turn.
 *
 * The bound is the architectural maximum instruction length, 15 bytes
 * (Intel SDM Vol. 2, the general instruction format; a longer encoding
 * raises #UD on a 386 and later).  An 8086 accepted any number, but
 * nothing LEGAL needs more than three prefixes -- one segment override,
 * one repeat, one LOCK -- so the cap cannot change what a legal
 * instruction decodes to, and the checks below say so both ways.
 *
 * A hang is not an exit status, so this section puts a deadline on
 * itself: before the fix the alarm fired and the run failed, which is
 * the only way an infinite loop reports itself.
 */

static void deadline(int sig)
{
	printf("FAIL %-44s (deadline: i86dec did not return)\n",
		"the prefix loop terminates");
	fflush(stdout);
	_exit(1);
}

/* ==================================================================
 * FUNCTION 13 IS DMA := 0080h.
 *
 * The native BDOS answers a reset by taking its own base page back as
 * the DMA address, which is the shim's base page and not the guest's.
 * A guest that resets the disk system and reads without setting a DMA
 * address of its own got its record written there -- the stub models
 * that by pointing its DMA at sbase -- and read stale content out of
 * its own buffer.  So the object here is WHERE the second file's
 * record lands.
 */
static void t_dma13(void)
{
	struct sfile *f;

	bsetup2();

	f = &sdisk[0];
	smkname(f->name, "ONE.DAT");
	f->used = 1;
	f->len = 128L;
	memset(f->d, 'A', 128);
	f = &sdisk[1];
	smkname(f->name, "TWO.DAT");
	f->used = 1;
	f->len = 128L;
	memset(f->d, 'B', 128);

	memset(dseg2 + 0x0100, 0, 36);
	smkname(dseg2 + 0x0100 + 1, "ONE.DAT");
	memset(dseg2 + 0x0140, 0, 36);
	smkname(dseg2 + 0x0140 + 1, "TWO.DAT");

	chk("fn 26 sets a DMA away from the default",
		bcall(26, 0x2000), B_RUN);
	chk("fn 15 opens the first file", bcall(15, 0x0100), B_RUN);
	chk("fn 20 reads it", bcall(20, 0x0100), B_RUN);
	chk("... into the guest's DMA", (long)(dseg2[0x2000] & 0xff),
		(long)'A');

	chk("fn 13 runs", bcall(13, 0), B_RUN);
	chk("fn 14 runs", bcall(14, 0), B_RUN);
	chk("fn 15 opens the second file", bcall(15, 0x0140), B_RUN);
	chk("fn 20 reads it", bcall(20, 0x0140), B_RUN);
	chk("a read after fn 13 lands at the guest's 0080h",
		(long)(dseg2[0x0080] & 0xff), (long)'B');
	chk("... and nowhere else", (long)(sbase[0] & 0xff), 0L);
	chk("... leaving the old DMA alone",
		(long)(dseg2[0x2000] & 0xff), (long)'A');
}

/*
 * A guest that prints while somebody types, and then reads its own
 * keys.  With the poll left on, the burst eats them; the seam turns it
 * off for the length of the run and hands the console back after.
 */
static void t_conmode(void)
{
	int i, got[4];

	sconn = 0;
	skqh = skqt = 0;
	skbchar = 0;
	sbrkctr = 0;
	bsetup();
	sysmode = SYS_CPM;
	sconmode = CM_NOSTOP << 1;	/* whatever the CCP was running in */
	i86bdosinit(&bm);
	chk("the run turns stop-scroll off", sconmode, CM_NOSTOP);

	/* Four keys waiting, then forty characters of output: five polls,
	 * which is enough to lose all four. */
	skqput("abcd");
	for (i = 0; i < 40; i++)
		bcall(2, (i16)'.');
	i86oflush();
	chk("the output went out", sconn, 40);

	for (i = 0; i < 4; i++) {
		bcall(6, (i16)0xff);
		got[i] = bm.r[R_AX] & 0xff;
	}
	chk("function 6 got the first key", got[0], 'a');
	chk("... the second", got[1], 'b');
	chk("... the third", got[2], 'c');
	chk("... and the fourth", got[3], 'd');

	chk("the guest's exit gives the console back", i86bdosfini(), 1);
	chk("... in the mode it found", sconmode, CM_NOSTOP << 1);
	chk("... once", i86bdosfini(), 0);

	/* The same burst with the poll on, which is what the console does
	 * to a guest the seam has not spoken for. */
	sconn = 0;
	skqh = skqt = 0;
	skbchar = 0;
	sbrkctr = 0;
	bsetup();
	sysmode = SYS_CPM;
	i86bdosinit(&bm);
	sconmode = 0;
	skqput("abcd");
	for (i = 0; i < 40; i++)
		bcall(2, (i16)'.');
	i86oflush();
	bcall(6, (i16)0xff);
	chk("the poll keeps only the last key it took",
		bm.r[R_AX] & 0xff, 'd');
	bcall(6, (i16)0xff);
	chk("... and the ones before it are gone", bm.r[R_AX] & 0xff, 0);

	i86bdosfini();
	sysmode = SYS_REC;
}

static void t_prefix(void)
{
	struct i86in	in;
	int		i, n;

	signal(SIGALRM, deadline);
	alarm(10);

	/* A whole segment of segment-override prefixes. */
	for (i = 0; i < 65536; i++)
		cseg[i] = (char)0x26;
	n = i86dec(cseg, (i16)0, &in);
	chk("a segment of 0x26 decodes to a bad instruction", (long)in.op,
		(long)I_BAD);
	ntest++;
	if (n <= 0 || n > 16)
		fail("a segment of 0x26 has a bounded length", (long)n, 16L);

	/* The same for each of the other prefix bytes, and for a mixture,
	 * because the loop took any of them. */
	for (i = 0; i < 65536; i++)
		cseg[i] = (char)0xf3;			/* REP		*/
	i86dec(cseg, (i16)0, &in);
	chk("a segment of 0xf3 decodes to a bad instruction", (long)in.op,
		(long)I_BAD);
	for (i = 0; i < 65536; i++)
		cseg[i] = (char)0xf0;			/* LOCK		*/
	i86dec(cseg, (i16)0, &in);
	chk("a segment of 0xf0 decodes to a bad instruction", (long)in.op,
		(long)I_BAD);
	for (i = 0; i < 65536; i++)
		cseg[i] = (char)(0x26 + 8 * (i & 3));	/* ES CS SS DS	*/
	i86dec(cseg, (i16)0, &in);
	chk("a segment of mixed overrides decodes to a bad instruction",
		(long)in.op, (long)I_BAD);

	/* ---- and now what must NOT change.  Every legal prefix
	   combination, ahead of a real instruction, decodes exactly as
	   it did: three prefixes is the most any 8086 instruction has a
	   use for. */

	memset(cseg, 0, 65536);
	cseg[0] = (char)0x26;			/* ES:		*/
	cseg[1] = (char)0x8b;			/* mov ax,[si]	*/
	cseg[2] = (char)0x04;
	chk("one override before mov ax,[si] is 3 bytes",
		(long)i86dec(cseg, (i16)0, &in), 3L);
	chk("... with the override taken", (long)in.seg, (long)S_ES);

	memset(cseg, 0, 65536);
	cseg[0] = (char)0xf0;			/* LOCK		*/
	cseg[1] = (char)0xf3;			/* REP		*/
	cseg[2] = (char)0x26;			/* ES:		*/
	cseg[3] = (char)0xa5;			/* movsw	*/
	chk("LOCK REP ES: movsw is 4 bytes",
		(long)i86dec(cseg, (i16)0, &in), 4L);
	chk("... with the override taken", (long)in.seg, (long)S_ES);
	ntest++;
	if (!(in.fl & IN_REP))
		fail("LOCK REP ES: movsw keeps IN_REP", 0, 1);
	ntest++;
	if (!(in.fl & IN_LOCK))
		fail("LOCK REP ES: movsw keeps IN_LOCK", 0, 1);

	/* The last override wins, which is the 8086's own rule and is
	 * what the loop was there for. */
	memset(cseg, 0, 65536);
	cseg[0] = (char)0x26;			/* ES:		*/
	cseg[1] = (char)0x2e;			/* CS:		*/
	cseg[2] = (char)0x36;			/* SS:		*/
	cseg[3] = (char)0x8b;
	cseg[4] = (char)0x04;
	chk("three overrides then mov is 5 bytes",
		(long)i86dec(cseg, (i16)0, &in), 5L);
	chk("... and the LAST one wins", (long)in.seg, (long)S_SS);

	/* The longest thing the cap must still accept: prefixes up to
	   the 15-byte architectural limit, opcode included. */
	memset(cseg, 0, 65536);
	for (i = 0; i < 14; i++)
		cseg[i] = (char)0x26;
	cseg[14] = (char)0x90;			/* nop		*/
	chk("fourteen prefixes and an opcode still decode to nop",
		(long)i86dec(cseg, (i16)0, &in), 15L);
	chk("... as a real instruction", (long)in.op, (long)I_NOP);

	/* One byte more than the limit is not an instruction. */
	memset(cseg, 0, 65536);
	for (i = 0; i < 15; i++)
		cseg[i] = (char)0x26;
	cseg[15] = (char)0x90;
	i86dec(cseg, (i16)0, &in);
	chk("fifteen prefixes and an opcode is not an instruction",
		(long)in.op, (long)I_BAD);

	alarm(0);
}

/* ---- DRI's DDT86.CMD debugging DRI's PIP.CMD ---- */

/* E loads PIP through function 59; G,D plants INT 3 at 000D and runs
 * PIP's prologue into it; X shows what the prologue made; T steps the
 * JMP with TF set; D dumps PIP's code group; ^C leaves. */
static void t_ddt86(const char *dir)
{
	struct ld L;
	struct i86in in;
	char path[512];
	struct sfile *fp;
	FILE *f;
	long nstep;
	int rc, brc;
	static const char *want[] = {
		"CS 2000:0000 2000:FFFF",
		"DS 3000:0000 3000:87FF",
		"*2000:000D",
		"--------- F002 0000 3000 0000 0184 0000 0000 0000 "
			"2000 3000 3000 3000 000D",
		"000D JMP    0112",
		"*2000:0112",
		"2000:0000 9C 58 FA 8C D9 8E D1 8D 26 84 01 50 9D E9 02 01",
		0
	};
	const char **w;

	sprintf(path, "%s/DDT86.CMD", dir);
	rc = ldread(path, &L);
	if (rc != CE_OK) {
		printf("i86test: %s: %s -- DDT86 skipped\n",
			path, rc < 0 ? "unreadable" : i86cerr(rc));
		ldfree(&L);
		return;
	}
	chk("ddt86 place", ldplace(&L, ""), CE_OK);

	memset(sdisk, 0, sizeof sdisk);
	memset(sfncount, 0, sizeof sfncount);
	sconn = 0;
	sdma = 0;
	sprintf(path, "%s/PIP.CMD", dir);
	fp = &sdisk[0];
	smkname(fp->name, "PIP.CMD");
	fp->used = 1;
	if ((f = fopen(path, "rb")) != 0) {
		fp->len = (long)fread(fp->d, 1, sizeof fp->d, f);
		fclose(f);
	}

	sysmode = SYS_CPM;
	i86bdosinit(&L.m);
	skeys = "EPIP.CMD\nG,D\nX\nT\nD2000:0,F\n\003";
	skeyeof = swboot = 0;
	brc = B_RUN;
	rc = X_OK;
	for (nstep = 0; nstep < 1000000L && !swboot; nstep++) {
		rc = i86step(&L.m, &in);
		if (rc == X_OK)
			continue;
		if (rc != X_INT)
			break;
		brc = i86bdos(&L.m);
		if (brc != B_RUN)
			break;
	}
	i86oflush();
	skeys = 0;
	scon[sconn] = 0;

	chk("ddt86 left on the ^C", swboot, 1);
	chk("ddt86 ended in a BDOS call", rc, X_INT);
	chk("ddt86 seam never refused", brc, B_RUN);
	chk("ddt86 never ran dry", skeyeof, 0);
	for (w = want; *w; w++)
		if (strstr(scon, *w) == 0) {
			printf("i86test: DDT86 console lacks \"%s\"\n", *w);
			chk("ddt86 console", 0, 1);
		}
	/* DDT86's own handlers, planted in paragraph 0 at its CS. */
	chk("ddt86 INT 1 vector segment",
		(zseg[6] & 0xff) | (zseg[7] & 0xff) << 8, 0x1000);
	chk("ddt86 INT 3 vector segment",
		(zseg[14] & 0xff) | (zseg[15] & 0xff) << 8, 0x1000);
	ldfree(&L);
}

/* ================================================================== */

/*
 * The `-x' / `-X' mode: load one .CMD with a command tail and run it
 * against the stub.  `-X' is the TOLERANT run: an executor refusal is
 * recorded by mnemonic and then STEPPED OVER, and a seam refusal is
 * recorded and answered with the 0FFFFh a real BDOS gives for a
 * function it does not have.  The tolerant run's later state is not a
 * correct one -- it is a way to see the SECOND gap, and the third,
 * instead of stopping at the first.
 */
static long xrefused[64];		/* by i86.h opcode class	*/
static const char *xrefname[64];
static long xfn[256];
static long xsegesc, xwindow, xbadn;
static int xtolerate;
static const char *xfname, *xfsrc;
static long xsegval[8];
static long xintno[256];

static void runx(const char *path, const char *tail)
{
	struct ld L;
	struct i86in in;
	long nstep, nref;
	int rc, brc, i;

	memset(xrefused, 0, sizeof xrefused);
	memset(xrefname, 0, sizeof xrefname);
	memset(xfn, 0, sizeof xfn);
	memset(xintno, 0, sizeof xintno);
	memset(xsegval, 0, sizeof xsegval);
	xsegesc = xwindow = xbadn = 0;
	rc = ldread(path, &L);
	if (rc != CE_OK) {
		printf("%s: %s\n", path,
			rc < 0 ? "unreadable" : i86cerr(rc));
		ldfree(&L);
		return;
	}
	rc = ldplace(&L, tail);
	if (rc != CE_OK) {
		printf("%s: place failed: %s\n", path, i86cerr(rc));
		ldfree(&L);
		return;
	}

	memset(sdisk, 0, sizeof sdisk);
	memset(sfncount, 0, sizeof sfncount);
	sconn = 0;
	sdma = 0;
	{
		struct sfile *fi = &sdisk[0];
		long k;

		smkname(fi->name, "VERIFY.IN");
		fi->used = 1;
		fi->len = 1024;
		for (k = 0; k < fi->len; k++)
			fi->d[k] = (char)(0x20 + (k % 0x5e));
	}
	/* An extra file, so that a utility gets past "No file" and into
	 * the code that is the point of running it. */
	if (xfname) {
		struct sfile *fx = &sdisk[1];
		FILE *fp;

		smkname(fx->name, xfname);
		fx->used = 1;
		fx->len = 0;
		if (xfsrc && (fp = fopen(xfsrc, "rb")) != 0) {
			fx->len = (long)fread(fx->d, 1, sizeof fx->d, fp);
			fclose(fp);
		}
	}

	sysmode = SYS_CPM;
	i86ninsn = i86nflag = 0;
	i86nsegslow = i86nsegbad = 0;
	i86bdosinit(&L.m);
	/* The version mask is the one seam answer a guest is allowed to
	 * disagree with out loud, so it is settable for a measurement. */
	if (getenv("I86VER"))
		i86ver = (i16)strtol(getenv("I86VER"), (char **)0, 0);
	/* Console input, for a program that has a command loop.  One
	 * newline-separated line per command; the run ends when the
	 * script does, so a prompt nobody answers cannot spin. */
	skeys = getenv("I86KEYS");
	skeyeof = 0;

	brc = B_RUN;
	rc = X_OK;
	nref = 0;
	for (nstep = 0; nstep < 5000000L; nstep++) {
		rc = i86step(&L.m, &in);
		if (rc == X_OK) {
			if (skeyeof)
				break;
			continue;
		}
		if (rc == X_INT) {
			brc = i86bdos(&L.m);
			if (brc == B_RUN)
				continue;
			if (xtolerate && (brc == B_FN || brc == B_VEC)
			 && ++nref < 2000) {
				if (brc == B_VEC)
					xintno[i86intno & 0xff]++;
				else if (i86bdosfn >= 0 && i86bdosfn < 256)
					xfn[i86bdosfn]++;
				L.m.r[R_AX] = (i16)0xffff;
				L.m.r[R_BX] = (i16)0xffff;
				brc = B_RUN;
				continue;
			}
			break;
		}
		if (!xtolerate || ++nref >= 2000)
			break;
		/* An executor refusal: name it, skip it, keep going. */
		if (rc == X_SEGESC) {
			if (xsegesc < 8)
				xsegval[xsegesc] = i86segbad & 0xffff;
			xsegesc++;
		} else if (rc == X_WINDOW) {
			xwindow++;
		} else if (rc == X_UNIMP || rc == X_BAD) {
			if (rc == X_BAD)
				xbadn++;
			if (in.op >= 0 && in.op < 64) {
				xrefused[in.op]++;
				xrefname[in.op] = i86mnem(&in);
			}
		} else {
			break;			/* X_HALT, X_WBOOT	*/
		}
		L.m.ip = (i16)(L.m.ip + (in.len ? in.len : 1));
	}
	i86oflush();

	printf("i86test: %s tail \"%s\": %ld instructions, %lu BDOS calls\n",
		path, tail, nstep, (unsigned long)i86nbdos);
	printf("i86test: %s stopped: %s, seam %s (fn %d) at cs:ip %04x:%04x\n",
		path,
		rc == X_OK ? "steplimit" :
		rc == X_UNIMP ? "X_UNIMP" :
		rc == X_BAD ? "X_BAD" :
		rc == X_HALT ? "X_HALT" :
		rc == X_SEGESC ? "X_SEGESC" :
		rc == X_WBOOT ? "X_WBOOT(exit)" :
		rc == X_WINDOW ? "X_WINDOW" : "X_INT",
		i86berr(), i86bdosfn,
		(unsigned)(L.m.sr[S_CS] & 0xffff), (unsigned)(L.m.ip & 0xffff));
	printf("i86test: %s BDOS functions used:", path);
	for (i = 0; i < (int)(sizeof sfncount / sizeof sfncount[0]); i++)
		if (sfncount[i])
			printf(" %d(%ld)", i, sfncount[i]);
	printf("\n");
	printf("i86test: %s segment slow %ld, refused %ld\n", path,
		(long)i86nsegslow, (long)i86nsegbad);
	if (xtolerate) {
		printf("i86test: %s REFUSED insn classes:", path);
		for (i = 0; i < 64; i++)
			if (xrefused[i])
				printf(" %s/op%d(%ld)",
					xrefname[i] ? xrefname[i] : "?",
					i, xrefused[i]);
		printf("\ni86test: %s REFUSED bdos fns:", path);
		for (i = 0; i < 256; i++)
			if (xfn[i])
				printf(" %d(%ld)", i, xfn[i]);
		printf("\ni86test: %s REFUSED int vectors:", path);
		for (i = 0; i < 256; i++)
			if (xintno[i])
				printf(" 0x%02x(%ld)", i, xintno[i]);
		printf("\ni86test: %s segesc %ld, window %ld, bad %ld;"
			" escaped paragraphs:", path, xsegesc, xwindow, xbadn);
		for (i = 0; i < 8 && i < xsegesc; i++)
			printf(" 0x%04lx", xsegval[i]);
		printf("\n");
	}
	printf("i86test: %s console: \"", path);
	for (i = 0; i < sconn; i++) {
		if (scon[i] == '\r')
			continue;
		if (scon[i] == '\n')
			printf("\\n");
		else if ((scon[i] & 0xff) < 32)
			printf("^%c", scon[i] + 64);
		else
			putchar(scon[i]);
	}
	printf("\"\n");
	ldfree(&L);
}

int main(argc, argv)
int argc;
char **argv;
{
	int i;

	if (argc > 2 && (strcmp(argv[1], "-x") == 0
			|| strcmp(argv[1], "-X") == 0)) {
		xtolerate = (argv[1][1] == 'X');
		xfname = argc > 4 ? argv[4] : 0;
		xfsrc = argc > 5 ? argv[5] : 0;
		runx(argv[2], argc > 3 ? argv[3] : "");
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "-g") == 0) {
		t_gsx86(argv[2]);
		printf("i86test: %d checks, %d failures\n", ntest, nfail);
		return (nfail != 0);
	}
	if (argc > 2 && strcmp(argv[1], "-c") == 0) {
		for (i = 2; i < argc; i++)
			sweep(argv[i]);
		return (0);
	}
	/* Both directories are REQUIRED, not optional.  A default that
	 * skipped sections 6 and 7 when they were not named would turn a
	 * broken make rule into a smaller passing run, which is the shape
	 * tests/relcheck.sh refuses for the release disk. */
	if (argc != 3) {
		fprintf(stderr, "usage: i86test <corpusdir> <fixturedir>\n");
		fprintf(stderr, "       i86test -c FILE.CMD ...\n");
		return (2);
	}

	t_lengths();
	t_allbytes();
	t_flags_byte();
	t_flags_word();
	t_incdec();
	t_lazycond();
	t_neg();
	t_delay();
	t_exec();
	t_string();
	t_far();
	t_segcheck();
	t_segslow();
	t_intvec();
	t_loader();
	t_corpus(argv[1]);
	t_fixtures(argv[2]);
	t_fcb();
	t_seam();
	t_dparms();
	t_pload(argv[1]);
	t_pip(argv[1]);
	t_submit(argv[1]);
	t_gencmd(argv[1], argv[2]);
	t_asm86(argv[1], argv[2]);
	t_stat(argv[1]);
	t_help(argv[1]);
	t_tod(argv[1]);
	t_gsx86(argv[2]);
	t_dmabound();
	t_random();
	t_dma13();
	t_conmode();
	t_prefix();
	t_ddt86(argv[1]);

	printf("i86test: %d checks, %d failures\n", ntest, nfail);
	/* K2's instruments, reported so that they are known to work.
	 * The RATE here is meaningless -- this harness reads the flags
	 * after nearly every instruction it executes, which is the
	 * opposite of what a program does.  The number that decides K2
	 * is this ratio taken over a real .CMD run, and that waits for
	 * the INT 0E0h seam. */
	if (i86ninsn)
		printf("i86test: instruments live: %lu instructions, %lu "
			"flag materialisations\n",
			(unsigned long)i86ninsn, (unsigned long)i86nflag);
	return (nfail != 0);
}
