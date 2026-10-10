/*
 * z80test.c -- host tests for the CP/M-80 shim's 8080/Z80 decoder,
 * executor, .COM loader and CALL 5 seam (src/cmd/z80dec.c, z80exec.c,
 * z80load.c, z80bdos.c).
 *
 * Build: cc -DHOSTCC -o z80test z80test.c ../src/cmd/z80dec.c \
 *		../src/cmd/z80exec.c ../src/cmd/z80load.c ../src/cmd/z80bdos.c
 *
 * Coverage includes instruction lengths and execution, every base opcode,
 * differential flags, explicit refusal cases, loader behavior, real COM
 * files, the CALL 5 convention, and complete DUMP and PIP runs against a stub
 * CP/M. The `-c` mode inventories any additional COM file.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "z80.h"
#include "gdpb.h"
#include "conmode.h"

static int nfail, ntest;

static void fail(const char *what, long got, long want)
{
	printf("FAIL %-44s got %ld (0x%lx) want %ld (0x%lx)\n",
		what, got, (unsigned long)got, want, (unsigned long)want);
	nfail++;
}

static void chk(const char *what, long got, long want)
{
	ntest++;
	if (got != want)
		fail(what, got, want);
}

/* One guest, reused by everything that does not need a fresh 64 KB. */
static char gmem[0x10000];
static struct z80 G;

static void gclear(void)
{
	memset(gmem, 0, sizeof gmem);
	memset(&G, 0, sizeof G);
	G.m = gmem;
	G.f = F_ONE;
	G.lz = LZ_NONE;
}

/* Put a byte string at 0x0100 and point PC at it. */
static void gcode(const unsigned char *b, int n)
{
	int i;

	for (i = 0; i < n; i++)
		gmem[0x100 + i] = (char)b[i];
	G.pc = 0x100;
}

/* ================================================================== */
/* 1. lengths							      */
/* ================================================================== */

/*
 * Transcribed from the encoding by hand, not generated from the
 * decoder -- a table the decoder produced would agree with it by
 * construction and prove nothing.  Each row is the byte string, its
 * length, and the mnemonic z80mnem() must name.
 */
struct lrow {
	unsigned char b[6];
	int n;
	int len;
	const char *m;
};

static struct lrow lrows[] = {
	/* --- one byte: the whole of 40-BF, and the odds and ends --- */
	{{0x00}, 1, 1, "nop"},
	{{0x40}, 1, 1, "mov"},			/* mov b,b		*/
	{{0x7e}, 1, 1, "mov"},			/* mov a,m		*/
	{{0x76}, 1, 1, "hlt"},			/* NOT mov m,m		*/
	{{0x80}, 1, 1, "add"},
	{{0x9f}, 1, 1, "sbb"},
	{{0xa6}, 1, 1, "ana"},			/* ana m		*/
	{{0xbe}, 1, 1, "cmp"},			/* cmp m		*/
	{{0x27}, 1, 1, "daa"},
	{{0x2f}, 1, 1, "cma"},
	{{0x37}, 1, 1, "stc"},
	{{0x3f}, 1, 1, "cmc"},
	{{0x07}, 1, 1, "rlc"},
	{{0x1f}, 1, 1, "rar"},
	{{0xe3}, 1, 1, "xthl"},
	{{0xe9}, 1, 1, "pchl"},
	{{0xeb}, 1, 1, "xchg"},
	{{0xf9}, 1, 1, "sphl"},
	{{0xc9}, 1, 1, "ret"},
	{{0xc0}, 1, 1, "rnz"},
	{{0xf8}, 1, 1, "rm"},
	{{0xc5}, 1, 1, "push"},
	{{0xf1}, 1, 1, "pop"},			/* pop psw		*/
	{{0xc7}, 1, 1, "rst"},
	{{0x09}, 1, 1, "dad"},
	{{0x33}, 1, 1, "inx"},			/* inx sp		*/
	{{0x3b}, 1, 1, "dcx"},
	{{0x34}, 1, 1, "inr"},			/* inr m		*/
	{{0x3d}, 1, 1, "dcr"},
	{{0x02}, 1, 1, "stax"},
	{{0x1a}, 1, 1, "ldax"},
	{{0xf3}, 1, 1, "di"},
	{{0xfb}, 1, 1, "ei"},
	/* --- two bytes: an 8-bit immediate --- */
	{{0x06, 0x41}, 2, 2, "mvi"},
	{{0x36, 0x41}, 2, 2, "mvi"},		/* mvi m,n		*/
	{{0x3e, 0xff}, 2, 2, "mvi"},
	{{0xc6, 0x01}, 2, 2, "add"},
	{{0xfe, 0x7f}, 2, 2, "cmp"},		/* cpi			*/
	{{0xd3, 0x10}, 2, 2, "out"},
	{{0xdb, 0x10}, 2, 2, "in"},
	/* --- three bytes: a 16-bit address or datum --- */
	{{0x01, 0x34, 0x12}, 3, 3, "lxi"},
	{{0x31, 0x00, 0xe4}, 3, 3, "lxi"},	/* lxi sp,nn		*/
	{{0x22, 0x34, 0x12}, 3, 3, "shld"},
	{{0x2a, 0x34, 0x12}, 3, 3, "lhld"},
	{{0x32, 0x34, 0x12}, 3, 3, "sta"},
	{{0x3a, 0x34, 0x12}, 3, 3, "lda"},
	{{0xc3, 0x00, 0x01}, 3, 3, "jmp"},
	{{0xca, 0x00, 0x01}, 3, 3, "jz"},
	{{0xfa, 0x00, 0x01}, 3, 3, "jm"},
	{{0xcd, 0x00, 0x01}, 3, 3, "call"},
	{{0xdc, 0x00, 0x01}, 3, 3, "cc"},
	{{0xec, 0x00, 0x01}, 3, 3, "cpe"},
	/* --- the Z80 base-map eight.  The relative branches are two
	 * bytes and their operand is a DISPLACEMENT, which is the length
	 * trap: read as an address they would be three. --- */
	{{0x08}, 1, 1, "ex af,af'"},
	{{0xd9}, 1, 1, "exx"},
	{{0x10, 0xfe}, 2, 2, "djnz"},
	{{0x18, 0x10}, 2, 2, "jr"},
	{{0x20, 0x10}, 2, 2, "jrnz"},
	{{0x28, 0x10}, 2, 2, "jrz"},
	{{0x30, 0x10}, 2, 2, "jrnc"},
	{{0x38, 0x10}, 2, 2, "jrc"},
	/* --- CB: always two, and one regular encoding throughout --- */
	{{0xcb, 0x00}, 2, 2, "rlc"},		/* rlc b		*/
	{{0xcb, 0x1e}, 2, 2, "rr"},		/* rr (hl)		*/
	{{0xcb, 0x27}, 2, 2, "sla"},		/* sla a		*/
	{{0xcb, 0x3f}, 2, 2, "srl"},		/* srl a		*/
	{{0xcb, 0x46}, 2, 2, "bit"},		/* bit 0,(hl)		*/
	{{0xcb, 0xbe}, 2, 2, "res"},		/* res 7,(hl)		*/
	{{0xcb, 0xff}, 2, 2, "set"},		/* set 7,a		*/
	/* --- ED: two, except the eight LD (nn),dd / LD dd,(nn) --- */
	{{0xed, 0xb0}, 2, 2, "ldir"},
	{{0xed, 0xb8}, 2, 2, "lddr"},
	{{0xed, 0xa1}, 2, 2, "cpi"},		/* the BLOCK compare	*/
	{{0xed, 0xb9}, 2, 2, "cpdr"},
	{{0xed, 0x42}, 2, 2, "sbc hl"},
	{{0xed, 0x5a}, 2, 2, "adc hl"},
	{{0xed, 0x44}, 2, 2, "neg"},
	{{0xed, 0x46}, 2, 2, "ed-group"},	/* im 0: still refused	*/
	{{0xed, 0x43, 0x34, 0x12}, 4, 4, "ld (nn),rp"},	/* ld (nn),bc	*/
	{{0xed, 0x4b, 0x34, 0x12}, 4, 4, "ld rp,(nn)"},	/* ld bc,(nn)	*/
	{{0xed, 0x7b, 0x34, 0x12}, 4, 4, "ld rp,(nn)"},	/* ld sp,(nn)	*/
	{{0xed, 0x63, 0x34, 0x12}, 4, 4, "ld (nn),rp"},	/* ld (nn),hl	*/
	/* --- our own escape lives in the ED space --- */
	{{0xed, 0xfe, 0x00}, 3, 3, "hook"},
	{{0xed, 0xfe, 0x11}, 3, 3, "hook"},
	/* --- DD/FD: the prefix adds one byte, and one more when the
	 * base form names (HL), because (HL) becomes (IX+d) --- */
	{{0xdd, 0x21, 0x34, 0x12}, 4, 4, "ix-group"},	/* ld ix,nn	*/
	{{0xfd, 0x21, 0x34, 0x12}, 4, 4, "iy-group"},	/* ld iy,nn	*/
	{{0xdd, 0x23}, 2, 2, "ix-group"},		/* inc ix	*/
	{{0xdd, 0xe5}, 2, 2, "ix-group"},		/* push ix	*/
	{{0xdd, 0x7e, 0x05}, 3, 3, "ix-group"},		/* ld a,(ix+5)	*/
	{{0xdd, 0x70, 0x05}, 3, 3, "ix-group"},		/* ld (ix+5),b	*/
	{{0xdd, 0x34, 0x05}, 3, 3, "ix-group"},		/* inc (ix+5)	*/
	{{0xdd, 0x86, 0x05}, 3, 3, "ix-group"},		/* add a,(ix+5)	*/
	{{0xdd, 0x36, 0x05, 0x41}, 4, 4, "ix-group"},	/* ld (ix+5),n	*/
	{{0xdd, 0x46, 0x05}, 3, 3, "ix-group"},		/* ld b,(ix+5)	*/
	/* 0x76 under a prefix is HLT, not `ld (ix+d),(hl)': it does not
	 * name memory in the operand sense and takes no displacement. */
	{{0xdd, 0x76}, 2, 2, "ix-group"},
	/* --- DD CB d op: the displacement comes BEFORE the operation
	 * byte.  Four bytes, and the only place in the architecture
	 * where the length rule reads backwards. --- */
	{{0xdd, 0xcb, 0x05, 0x06}, 4, 4, "ixcb-group"},
	{{0xfd, 0xcb, 0xfb, 0xc6}, 4, 4, "iycb-group"},
	/* --- a prefix in front of a prefix is DISCARDED and is ONE
	 * byte.  DD DD 21 nn nn is `ld ix,nn' with four T-states
	 * wasted; DD ED B0 is a plain LDIR.  Read as two bytes, the
	 * second leaves B0 to decode as `ora b' and the stream is gone.
	 * --- */
	{{0xdd, 0xdd, 0x21, 0x34, 0x12}, 5, 1, "ix-group"},
	{{0xdd, 0xfd, 0x21, 0x34, 0x12}, 5, 1, "ix-group"},
	{{0xfd, 0xdd}, 2, 1, "iy-group"},
	{{0xdd, 0xed, 0xb0}, 3, 1, "ix-group"},
	{{0xfd, 0xed, 0xb0}, 3, 1, "iy-group"}
};

static void t_lengths(void)
{
	struct z80in in;
	char what[80];
	int i, n, got;

	n = (int)(sizeof lrows / sizeof lrows[0]);
	for (i = 0; i < n; i++) {
		gclear();
		gcode(lrows[i].b, lrows[i].n);
		got = z80dec(gmem, (z16)0x100, &in);
		sprintf(what, "len[%02x %02x %02x]", lrows[i].b[0],
			lrows[i].b[1], lrows[i].b[2]);
		chk(what, got, lrows[i].len);
		chk(what, (long)in.len, lrows[i].len);
		ntest++;
		if (strcmp(z80mnem(&in), lrows[i].m) != 0) {
			printf("FAIL %-44s got \"%s\" want \"%s\"\n",
				what, z80mnem(&in), lrows[i].m);
			nfail++;
		}
	}
}

/* ================================================================== */
/* 2. the whole base map					      */
/* ================================================================== */

/*
 * Every one of the 256 base bytes decodes, and none of them decodes to
 * Z_BAD.  That is not a formality: it is the property that makes
 * Z_UNIMP mean "we have not written this" and Z_BAD mean "these bytes
 * are not code", and if any base byte ever returned Z_BAD the two
 * findings would be confused for good.
 *
 * The (HL) census is here too, checked against a rule written the other
 * way round from usesm()'s: an opcode names memory when it is 0x34,
 * 0x35 or 0x36, or when it is in 0x40-0xBF with the low field 6 and is
 * not 0x76, or when it is in 0x70-0x77 and is not 0x76.
 */
static void t_allbytes(void)
{
	struct z80in in;
	unsigned char b[4];
	int op, bad, nmem, want, got;

	bad = 0;
	nmem = 0;
	for (op = 0; op < 256; op++) {
		b[0] = (unsigned char)op;
		b[1] = 0x00;
		b[2] = 0x00;
		b[3] = 0x00;
		gclear();
		gcode(b, 4);
		z80dec(gmem, (z16)0x100, &in);
		if (in.op == Z_BAD)
			bad++;
		if (in.len < 1 || in.len > 4)
			bad++;
		want = 0;
		if (op == 0x34 || op == 0x35 || op == 0x36)
			want = 1;
		else if (op >= 0x40 && op <= 0xbf && op != 0x76
		      && ((op & 7) == 6 || (op >= 0x70 && op <= 0x77)))
			want = 1;
		got = (in.fl & ZF_MEM) != 0 && in.op != Z_LDA
		   && in.op != Z_STA && in.op != Z_LHLD && in.op != Z_SHLD
		   && in.op != Z_XTHL && in.op != Z_LDAX && in.op != Z_STAX;
		if (want != got)
			nmem++;
	}
	chk("all 256 base bytes decode", bad, 0);
	chk("(HL) census agrees", nmem, 0);

	/* ED FE nn with nn past HOOK_MAX is the one thing that IS
	 * Z_BAD: it is our own escape used with a hook we never
	 * planted, and it must not be mistaken for an ED instruction. */
	b[0] = 0xed; b[1] = 0xfe; b[2] = (unsigned char)(HOOK_MAX + 1);
	gclear();
	gcode(b, 3);
	z80dec(gmem, (z16)0x100, &in);
	chk("ED FE with a bad hook is Z_BAD", in.op, Z_BAD);
	chk("... and still three bytes long", in.len, 3);
}

/* ================================================================== */
/* 3. flags, differentially					      */
/* ================================================================== */

/*
 * The reference, written from the 8080's own definitions in wide
 * arithmetic.  It shares no line with z80exec.c and is deliberately
 * clumsier: the point is that the two agree, not that either is neat.
 */
static int rpar(int v)
{
	int n, i;

	n = 0;
	for (i = 0; i < 8; i++)
		if (v & (1 << i))
			n++;
	return ((n & 1) == 0);
}

static int rszp(int r)
{
	int f;

	f = F_ONE;
	if ((r & 0xff) == 0)
		f |= F_ZE;
	if (r & 0x80)
		f |= F_SI;
	if (rpar(r & 0xff))
		f |= F_PA;
	return (f);
}

/*
 * aop 0..7 = add adc sub sbb ana xra ora cmp.  Returns the flag byte;
 * *res gets the byte the accumulator would take (the caller ignores it
 * for CMP).
 */
static int ralu(int aop, int a, int b, int cin, int *res)
{
	int r, f, half;

	a &= 0xff;
	b &= 0xff;
	cin = cin ? 1 : 0;
	switch (aop) {
	case 0: case 1:				/* ADD, ADC		*/
		if (aop == 0)
			cin = 0;
		r = a + b + cin;
		f = rszp(r);
		if (r > 0xff)
			f |= F_CY;
		/* Half carry: the carry out of bit 3 of the same sum,
		 * done on the low nibbles alone. */
		if (((a & 0x0f) + (b & 0x0f) + cin) > 0x0f)
			f |= F_AC;
		break;
	case 2: case 3: case 7:			/* SUB, SBB, CMP	*/
		if (aop != 3)
			cin = 0;
		r = a - b - cin;
		f = rszp(r);
		if (r < 0)
			f |= F_CY;
		/* The 8080 does subtraction as a + ~b + 1 and reports
		 * the ADDITION's half carry, so AC is set when there was
		 * NO half borrow.  Written here as the borrow and then
		 * inverted, which is the definition read the other way. */
		half = (a & 0x0f) - (b & 0x0f) - cin;
		if (half >= 0)
			f |= F_AC;
		break;
	case 4:					/* ANA			*/
		r = a & b;
		f = rszp(r);
		if ((a | b) & 0x08)
			f |= F_AC;
		break;
	case 5:					/* XRA			*/
		r = a ^ b;
		f = rszp(r);
		break;
	default:				/* ORA			*/
		r = a | b;
		f = rszp(r);
		break;
	}
	*res = r & 0xff;
	return (f & ~F_Z80X);
}

/* The eight ALU opcodes against B, and the eight immediate forms. */
static unsigned char aluop[8] = {0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8};
static unsigned char aluim[8] = {0xc6, 0xce, 0xd6, 0xde, 0xe6, 0xee, 0xf6, 0xfe};

static void t_flags_alu(void)
{
	struct z80in in;
	unsigned char b[2];
	char what[80];
	long cases;
	int aop, a, s, cin, wf, wr, bad, badr, badi;

	cases = 0;
	for (aop = 0; aop < 8; aop++) {
		bad = 0;
		badr = 0;
		badi = 0;
		for (cin = 0; cin < 2; cin++)
		for (a = 0; a < 256; a++)
		for (s = 0; s < 256; s++) {
			wf = ralu(aop, a, s, cin, &wr);

			/* register form: ALU A,B */
			gclear();
			b[0] = aluop[aop];
			gcode(b, 1);
			G.a = (z8)a;
			z80setr(&G, R_B, s);
			G.f = (z8)(F_ONE | (cin ? F_CY : 0));
			if (z80step(&G, &in) != X_OK)
				bad++;
			else {
				if ((int)z80flags(&G) != wf)
					bad++;
				if ((int)G.a != (aop == 7 ? a : wr))
					badr++;
			}

			/* immediate form: ALU A,n -- the same arithmetic
			 * reached through a different decode, which is
			 * where an operand mix-up would show. */
			gclear();
			b[0] = aluim[aop];
			b[1] = (unsigned char)s;
			gcode(b, 2);
			G.a = (z8)a;
			G.f = (z8)(F_ONE | (cin ? F_CY : 0));
			if (z80step(&G, &in) != X_OK)
				badi++;
			else if ((int)z80flags(&G) != wf)
				badi++;
			cases += 2;
		}
		sprintf(what, "alu %d flags (register)", aop);
		chk(what, bad, 0);
		sprintf(what, "alu %d result", aop);
		chk(what, badr, 0);
		sprintf(what, "alu %d flags (immediate)", aop);
		chk(what, badi, 0);
	}
	printf("z80test: ALU differential: %ld cases\n", cases);
}

/*
 * INR and DCR are separate classes because they PRESERVE carry, which
 * is the one place the 8080's flag rules are not uniform.  Both are
 * checked against the reference with carry set and clear, and the
 * check that carry survived is a check of its own.
 */
static void t_flags_incdec(void)
{
	struct z80in in;
	unsigned char b[1];
	int v, cin, wf, wr, bad, badcy;

	bad = 0;
	badcy = 0;
	for (cin = 0; cin < 2; cin++)
	for (v = 0; v < 256; v++) {
		/* INR B */
		wf = ralu(0, v, 1, 0, &wr);
		wf = (wf & ~F_CY) | (cin ? F_CY : 0);
		gclear();
		b[0] = 0x04;
		gcode(b, 1);
		z80setr(&G, R_B, v);
		G.f = (z8)(F_ONE | (cin ? F_CY : 0));
		z80step(&G, &in);
		if ((int)z80flags(&G) != wf)
			bad++;
		if (z80getr(&G, R_B) != ((v + 1) & 0xff))
			bad++;
		if (((G.f & F_CY) != 0) != cin)
			badcy++;

		/* DCR B */
		wf = ralu(2, v, 1, 0, &wr);
		wf = (wf & ~F_CY) | (cin ? F_CY : 0);
		gclear();
		b[0] = 0x05;
		gcode(b, 1);
		z80setr(&G, R_B, v);
		G.f = (z8)(F_ONE | (cin ? F_CY : 0));
		z80step(&G, &in);
		if ((int)z80flags(&G) != wf)
			bad++;
		if (z80getr(&G, R_B) != ((v - 1) & 0xff))
			bad++;
		if (((G.f & F_CY) != 0) != cin)
			badcy++;
	}
	chk("inr/dcr flags", bad, 0);
	chk("inr/dcr preserve carry", badcy, 0);
}

/*
 * DAA, over every one of its 1,024 inputs.
 *
 * The reference is the TWO-STEP form out of the 8080 manual -- "the
 * accumulator is incremented by six", then "if the most significant
 * four bits NOW represent a number greater than nine ... incremented by
 * six" -- where z80exec.c computes the whole correction from the
 * original accumulator in one pass.  The two are equivalent and the
 * equivalence is not obvious, which is exactly why the reference is
 * written the other way.
 *
 * The equivalence turns on the step-one add being an EIGHT-BIT add
 * whose carry out is a carry out of the accumulator.  Written without
 * that -- in wide arithmetic, so that 0xFF + 6 is 0x105 and the high
 * nibble tests as 0 -- this reference disagrees with z80exec.c on
 * exactly 24 of the 1,024 inputs: A in 0xFA..0xFF, both carries, both
 * half-carries, where the low correction wraps the accumulator.  It
 * did, on the first run, and z80exec.c was right.  That is what this
 * section is for and the case is left named here so the next person
 * writing a DAA knows where to look.
 *
 * The 8080's DAA SETS carry and never clears it.  That is the detail
 * multi-byte BCD addition depends on and the detail an emulator written
 * from a summary gets wrong, so it is a check of its own.
 */
static void t_flags_daa(void)
{
	struct z80in in;
	unsigned char b[1];
	int a, cy, ac, x, f, cout, bad, badcy;

	bad = 0;
	badcy = 0;
	for (a = 0; a < 256; a++)
	for (cy = 0; cy < 2; cy++)
	for (ac = 0; ac < 2; ac++) {
		x = a;
		f = F_ONE;
		cout = cy;
		if ((x & 0x0f) > 9 || ac) {
			if (((x & 0x0f) + 6) > 0x0f)
				f |= F_AC;
			if (x + 6 > 0xff)
				cout = 1;	/* an EIGHT-bit add	*/
			x = (x + 6) & 0xff;
		}
		if (((x >> 4) & 0x0f) > 9 || cout) {
			x = (x + 0x60) & 0xff;
			cout = 1;
		}
		if (cout)
			f |= F_CY;
		f |= (rszp(x) & (F_ZE | F_SI | F_PA));

		gclear();
		b[0] = 0x27;
		gcode(b, 1);
		G.a = (z8)a;
		G.f = (z8)(F_ONE | (cy ? F_CY : 0) | (ac ? F_AC : 0));
		z80step(&G, &in);
		if ((int)G.a != x)
			bad++;
		if ((int)z80flags(&G) != f)
			bad++;
		if (cy && !(G.f & F_CY))
			badcy++;
	}
	chk("daa over all 1024 inputs", bad, 0);
	chk("daa never clears carry", badcy, 0);
}

/*
 * DAD is the only 16-bit flag-setting instruction on the part and it
 * touches carry alone.  So the check is in two halves: the carry is
 * right, and the OTHER four flags survived a pending lazy record --
 * which is the failure a lazy scheme invites and which no single-step
 * test of DAD on its own would see.
 */
static void t_flags_dad(void)
{
	struct z80in in;
	unsigned char b[3];
	long hl, de;
	int bad, badkeep, f0;

	bad = 0;
	badkeep = 0;
	for (hl = 0; hl < 0x10000L; hl += 337)
	for (de = 0; de < 0x10000L; de += 1009) {
		gclear();
		b[0] = 0x19;			/* dad d		*/
		gcode(b, 1);
		G.rp[P_HL] = (z16)hl;
		G.rp[P_DE] = (z16)de;
		z80step(&G, &in);
		if (G.rp[P_HL] != (z16)((hl + de) & 0xffff))
			bad++;
		if (((z80flags(&G) & F_CY) != 0) != ((hl + de) > 0xffffL))
			bad++;
	}
	chk("dad result and carry", bad, 0);

	/* An ORA A (which clears carry and sets S/Z/P) followed by a DAD
	 * that carries: the four surviving flags must be the ORA's. */
	gclear();
	b[0] = 0xb7;				/* ora a		*/
	b[1] = 0x19;				/* dad d		*/
	gcode(b, 2);
	G.a = 0x81;
	G.rp[P_HL] = 0xffff;
	G.rp[P_DE] = 0x0001;
	z80step(&G, &in);
	f0 = (int)z80flags(&G);
	gclear();
	gcode(b, 2);
	G.a = 0x81;
	G.rp[P_HL] = 0xffff;
	G.rp[P_DE] = 0x0001;
	z80step(&G, &in);			/* ora			*/
	z80step(&G, &in);			/* dad			*/
	if ((z80flags(&G) & (F_SI | F_ZE | F_PA | F_AC))
	 != (f0 & (F_SI | F_ZE | F_PA | F_AC)))
		badkeep++;
	chk("dad keeps S/Z/P/AC across a lazy record", badkeep, 0);
	chk("dad carried", (long)(G.f & F_CY), (long)F_CY);
	chk("dad wrapped", (long)G.rp[P_HL], 0L);
}

/*
 * The four rotates.  Only carry is affected, and the 8080's naming is
 * the trap: RLC/RRC do NOT go through carry and RAL/RAR do.  Checked
 * over all 256 accumulators and both carries, with the surviving flags
 * checked as well.
 */
static void t_flags_rot(void)
{
	struct z80in in;
	unsigned char b[1];
	static unsigned char rop[4] = {0x07, 0x0f, 0x17, 0x1f};
	int i, a, cy, wa, wc, bad, badkeep;

	bad = 0;
	badkeep = 0;
	for (i = 0; i < 4; i++)
	for (a = 0; a < 256; a++)
	for (cy = 0; cy < 2; cy++) {
		switch (i) {
		case 0: wc = (a >> 7) & 1; wa = ((a << 1) | wc) & 0xff; break;
		case 1: wc = a & 1; wa = ((a >> 1) | (wc << 7)) & 0xff; break;
		case 2: wc = (a >> 7) & 1; wa = ((a << 1) | cy) & 0xff; break;
		default: wc = a & 1; wa = ((a >> 1) | (cy << 7)) & 0xff; break;
		}
		gclear();
		b[0] = rop[i];
		gcode(b, 1);
		G.a = (z8)a;
		G.f = (z8)(F_ONE | F_ZE | F_SI | F_PA | F_AC
			 | (cy ? F_CY : 0));
		z80step(&G, &in);
		if ((int)G.a != wa)
			bad++;
		if (((z80flags(&G) & F_CY) != 0) != wc)
			bad++;
		if ((G.f & (F_ZE | F_SI | F_PA | F_AC))
		 != (F_ZE | F_SI | F_PA | F_AC))
			badkeep++;
	}
	chk("rotate results and carry", bad, 0);
	chk("rotate touches nothing but carry", badkeep, 0);
}

/*
 * PUSH PSW / POP PSW round trip.  Period code compares a pushed flag
 * byte, so the constant bits matter: bit 1 reads back as one and bits 5
 * and 3 as zero on an 8080, whatever was pushed.
 */
static void t_flags_psw(void)
{
	struct z80in in;
	unsigned char b[2];
	int v, bad;

	bad = 0;
	for (v = 0; v < 256; v++) {
		gclear();
		b[0] = 0xf5;			/* push psw		*/
		b[1] = 0xf1;			/* pop psw		*/
		gcode(b, 2);
		G.rp[P_SP] = 0x2000;
		G.a = 0x5a;
		G.f = (z8)((v | F_ONE) & ~F_Z80X);
		z80step(&G, &in);
		if ((gmem[0x1ffe] & 0xff) != (int)G.f)
			bad++;
		if ((gmem[0x1fff] & 0xff) != 0x5a)
			bad++;
		G.a = 0;
		G.f = 0;
		z80step(&G, &in);
		if ((int)G.a != 0x5a)
			bad++;
		if ((int)G.f != (int)((v | F_ONE) & ~F_Z80X))
			bad++;
	}
	chk("push/pop psw round trip", bad, 0);

	/* And the masking: a flag byte with bits 5 and 3 set -- which is
	 * what a Z80 would have pushed -- comes back with them clear. */
	gclear();
	b[0] = 0xf1;				/* pop psw		*/
	gcode(b, 1);
	G.rp[P_SP] = 0x2000;
	gmem[0x2000] = (char)0xff;
	gmem[0x2001] = (char)0x11;
	z80step(&G, &in);
	chk("pop psw masks the Z80 bits", (long)G.f,
		(long)(0xff & ~F_Z80X));
	chk("pop psw takes A from the high byte", (long)G.a, 0x11L);
}

/* ================================================================== */
/* 4. execution							      */
/* ================================================================== */

/*
 * One test per implemented class: state in, one z80step(), state out.
 * The interesting cases are the ones where the 8080's encoding is not
 * uniform -- r = 6 meaning (HL), rp = 3 meaning SP everywhere except
 * PUSH and POP where it means PSW, RST's target, and the wraparound the
 * whole memory model rests on.
 */
static void t_exec(void)
{
	struct z80in in;
	unsigned char b[6];

	/* MOV r,r' and MOV r,M / MOV M,r */
	gclear();
	b[0] = 0x41;				/* mov b,c		*/
	gcode(b, 1);
	z80setr(&G, R_C, 0x37);
	chk("mov b,c", z80step(&G, &in), X_OK);
	chk("mov b,c value", z80getr(&G, R_B), 0x37);
	chk("mov b,c pc", (long)G.pc, 0x101L);

	gclear();
	b[0] = 0x7e;				/* mov a,m		*/
	gcode(b, 1);
	G.rp[P_HL] = 0x4000;
	gmem[0x4000] = (char)0xa5;
	z80step(&G, &in);
	chk("mov a,m", (long)G.a, 0xa5L);

	gclear();
	b[0] = 0x71;				/* mov m,c		*/
	gcode(b, 1);
	G.rp[P_HL] = 0x4000;
	z80setr(&G, R_C, 0x5a);
	z80step(&G, &in);
	chk("mov m,c", (long)(gmem[0x4000] & 0xff), 0x5aL);

	/* MVI M,n -- the one MVI that writes memory */
	gclear();
	b[0] = 0x36; b[1] = 0x99;
	gcode(b, 2);
	G.rp[P_HL] = 0x4001;
	z80step(&G, &in);
	chk("mvi m,n", (long)(gmem[0x4001] & 0xff), 0x99L);
	chk("mvi m,n pc", (long)G.pc, 0x102L);

	/* LXI / INX / DCX with rp = 3 meaning SP */
	gclear();
	b[0] = 0x31; b[1] = 0x00; b[2] = 0xe4;	/* lxi sp,0e400h	*/
	b[3] = 0x33;				/* inx sp		*/
	b[4] = 0x3b; b[5] = 0x3b;		/* dcx sp; dcx sp	*/
	gcode(b, 6);
	z80step(&G, &in);
	chk("lxi sp", (long)G.rp[P_SP], 0xe400L);
	z80step(&G, &in);
	chk("inx sp", (long)G.rp[P_SP], 0xe401L);
	z80step(&G, &in);
	z80step(&G, &in);
	chk("dcx sp twice", (long)G.rp[P_SP], 0xe3ffL);

	/* INX wraps in 16 bits and touches no flag */
	gclear();
	b[0] = 0x23;				/* inx h		*/
	gcode(b, 1);
	G.rp[P_HL] = 0xffff;
	G.f = (z8)(F_ONE | F_CY | F_ZE);
	z80step(&G, &in);
	chk("inx h wraps", (long)G.rp[P_HL], 0L);
	chk("inx h keeps flags", (long)z80flags(&G),
		(long)(F_ONE | F_CY | F_ZE));

	/* LDAX / STAX / LDA / STA / LHLD / SHLD */
	gclear();
	b[0] = 0x1a;				/* ldax d		*/
	gcode(b, 1);
	G.rp[P_DE] = 0x4002;
	gmem[0x4002] = 0x11;
	z80step(&G, &in);
	chk("ldax d", (long)G.a, 0x11L);

	gclear();
	b[0] = 0x02;				/* stax b		*/
	gcode(b, 1);
	G.rp[P_BC] = 0x4003;
	G.a = 0x22;
	z80step(&G, &in);
	chk("stax b", (long)(gmem[0x4003] & 0xff), 0x22L);

	gclear();
	b[0] = 0x2a; b[1] = 0x00; b[2] = 0x40;	/* lhld 4000h		*/
	gcode(b, 3);
	gmem[0x4000] = 0x34;
	gmem[0x4001] = 0x12;
	z80step(&G, &in);
	chk("lhld is little-endian", (long)G.rp[P_HL], 0x1234L);

	gclear();
	b[0] = 0x22; b[1] = 0x00; b[2] = 0x40;	/* shld 4000h		*/
	gcode(b, 3);
	G.rp[P_HL] = 0xbeef;
	z80step(&G, &in);
	chk("shld low byte", (long)(gmem[0x4000] & 0xff), 0xefL);
	chk("shld high byte", (long)(gmem[0x4001] & 0xff), 0xbeL);

	/* XCHG, PCHL, SPHL, XTHL */
	gclear();
	b[0] = 0xeb;
	gcode(b, 1);
	G.rp[P_HL] = 0x1111;
	G.rp[P_DE] = 0x2222;
	z80step(&G, &in);
	chk("xchg hl", (long)G.rp[P_HL], 0x2222L);
	chk("xchg de", (long)G.rp[P_DE], 0x1111L);

	gclear();
	b[0] = 0xe9;
	gcode(b, 1);
	G.rp[P_HL] = 0x1234;
	z80step(&G, &in);
	chk("pchl", (long)G.pc, 0x1234L);

	gclear();
	b[0] = 0xf9;
	gcode(b, 1);
	G.rp[P_HL] = 0x4321;
	z80step(&G, &in);
	chk("sphl", (long)G.rp[P_SP], 0x4321L);

	gclear();
	b[0] = 0xe3;
	gcode(b, 1);
	G.rp[P_SP] = 0x2000;
	G.rp[P_HL] = 0xaaaa;
	gmem[0x2000] = 0x55;
	gmem[0x2001] = 0x55;
	z80step(&G, &in);
	chk("xthl hl", (long)G.rp[P_HL], 0x5555L);
	chk("xthl memory", (long)(gmem[0x2000] & 0xff), 0xaaL);

	/* PUSH/POP with rp = 3, which is PSW and not SP */
	gclear();
	b[0] = 0xc5; b[1] = 0xc1;		/* push b; pop b	*/
	gcode(b, 2);
	G.rp[P_SP] = 0x2000;
	G.rp[P_BC] = 0x1234;
	z80step(&G, &in);
	chk("push b sp", (long)G.rp[P_SP], 0x1ffeL);
	chk("push b low", (long)(gmem[0x1ffe] & 0xff), 0x34L);
	chk("push b high", (long)(gmem[0x1fff] & 0xff), 0x12L);
	G.rp[P_BC] = 0;
	z80step(&G, &in);
	chk("pop b", (long)G.rp[P_BC], 0x1234L);
	chk("pop b sp", (long)G.rp[P_SP], 0x2000L);

	/* CALL / RET, and the return address is past the CALL */
	gclear();
	b[0] = 0xcd; b[1] = 0x00; b[2] = 0x40;
	gcode(b, 3);
	G.rp[P_SP] = 0x2000;
	z80step(&G, &in);
	chk("call pc", (long)G.pc, 0x4000L);
	chk("call pushed return", (long)(((gmem[0x1fff] & 0xff) << 8)
		| (gmem[0x1ffe] & 0xff)), 0x103L);
	gmem[0x4000] = (char)0xc9;		/* ret			*/
	G.pc = 0x4000;
	z80step(&G, &in);
	chk("ret pc", (long)G.pc, 0x103L);
	chk("ret sp", (long)G.rp[P_SP], 0x2000L);

	/* RST n: the target is n * 8 */
	gclear();
	b[0] = 0xdf;				/* rst 3		*/
	gcode(b, 1);
	G.rp[P_SP] = 0x2000;
	z80step(&G, &in);
	chk("rst 3 target", (long)G.pc, 0x18L);

	/* The eight conditions, on JMP.  Set up so that exactly the
	 * true ones are taken, one flag word at a time. */
	{
		static unsigned char jop[8] = {0xc2, 0xca, 0xd2, 0xda,
					       0xe2, 0xea, 0xf2, 0xfa};
		static int fset[8] = {0, F_ZE, 0, F_CY, 0, F_PA, 0, F_SI};
		int i, taken, bad;

		bad = 0;
		for (i = 0; i < 8; i++) {
			int j;

			for (j = 0; j < 2; j++) {
				gclear();
				b[0] = jop[i]; b[1] = 0x00; b[2] = 0x40;
				gcode(b, 3);
				G.f = (z8)(F_ONE | (j ? fset[i ^ 1] : 0));
				/* fset is indexed so that i and i^1 are the
				 * pair "not X" and "X"; j = 1 sets the flag
				 * the pair tests. */
				G.f = (z8)(F_ONE
					 | (j ? (i & 1 ? fset[i] : fset[i | 1])
					      : 0));
				z80step(&G, &in);
				taken = (G.pc == 0x4000);
				/* even i is "not set", odd i is "set" */
				if (taken != ((i & 1) ? j : !j))
					bad++;
			}
		}
		chk("the eight jump conditions", bad, 0);
	}

	/* The Z80 four */
	gclear();
	b[0] = 0x18; b[1] = 0x05;		/* jr +5		*/
	gcode(b, 2);
	z80step(&G, &in);
	chk("jr forward", (long)G.pc, 0x107L);

	gclear();
	b[0] = 0x18; b[1] = 0xfe;		/* jr -2, to itself	*/
	gcode(b, 2);
	z80step(&G, &in);
	chk("jr backward", (long)G.pc, 0x100L);

	gclear();
	b[0] = 0x10; b[1] = 0xfe;		/* djnz -2		*/
	gcode(b, 2);
	z80setr(&G, R_B, 3);
	z80step(&G, &in);
	chk("djnz decrements b", z80getr(&G, R_B), 2);
	chk("djnz loops", (long)G.pc, 0x100L);
	z80setr(&G, R_B, 1);
	G.pc = 0x100;
	z80step(&G, &in);
	chk("djnz falls through at zero", (long)G.pc, 0x102L);

	gclear();
	b[0] = 0x08;				/* ex af,af'		*/
	gcode(b, 2);
	G.a = 0x11;
	G.f = (z8)(F_ONE | F_CY);
	G.aa = 0x22;
	G.af = F_ONE;
	z80step(&G, &in);
	chk("ex af,af' a", (long)G.a, 0x22L);
	chk("ex af,af' a'", (long)G.aa, 0x11L);
	chk("ex af,af' f'", (long)G.af, (long)(F_ONE | F_CY));

	gclear();
	b[0] = 0xd9;				/* exx			*/
	gcode(b, 1);
	G.rp[P_BC] = 0x1111;
	G.rp[P_DE] = 0x2222;
	G.rp[P_HL] = 0x3333;
	G.rp[P_SP] = 0x4444;
	G.arp[0] = 0xaaaa;
	z80step(&G, &in);
	chk("exx bc", (long)G.rp[P_BC], 0xaaaaL);
	chk("exx bc'", (long)G.arp[0], 0x1111L);
	chk("exx leaves sp alone", (long)G.rp[P_SP], 0x4444L);

	/* CMA touches no flag; STC sets carry; CMC complements it */
	gclear();
	b[0] = 0x2f;
	gcode(b, 1);
	G.a = 0x0f;
	G.f = (z8)(F_ONE | F_CY | F_ZE);
	z80step(&G, &in);
	chk("cma", (long)G.a, 0xf0L);
	chk("cma keeps flags", (long)z80flags(&G),
		(long)(F_ONE | F_CY | F_ZE));

	gclear();
	b[0] = 0x37; b[1] = 0x3f;		/* stc; cmc		*/
	gcode(b, 2);
	z80step(&G, &in);
	chk("stc", (long)(G.f & F_CY), (long)F_CY);
	z80step(&G, &in);
	chk("cmc", (long)(G.f & F_CY), 0L);

	/* THE MEMORY MODEL.  A guest address that runs off 0xFFFF wraps
	 * to 0, which on the target the hardware does for free
	 * (Z80-SHIM-FEASIBILITY.md §1.1) and here a (z16) cast does.
	 * Checked on the three things that can cross the boundary: a
	 * 16-bit load, the stack, and the instruction stream. */
	gclear();
	b[0] = 0x2a; b[1] = 0xff; b[2] = 0xff;	/* lhld 0ffffh		*/
	gcode(b, 3);
	gmem[0xffff] = 0x78;
	gmem[0x0000] = 0x56;
	z80step(&G, &in);
	chk("lhld wraps at 0xffff", (long)G.rp[P_HL], 0x5678L);

	gclear();
	b[0] = 0xc5;				/* push b		*/
	gcode(b, 1);
	G.rp[P_SP] = 0x0001;
	G.rp[P_BC] = 0x1234;
	z80step(&G, &in);
	chk("push wraps the stack", (long)G.rp[P_SP], 0xffffL);
	chk("push wrapped low byte", (long)(gmem[0xffff] & 0xff), 0x34L);
	chk("push wrapped high byte", (long)(gmem[0x0000] & 0xff), 0x12L);

	gclear();
	gmem[0xffff] = 0x01;			/* lxi b,1234h ...	*/
	gmem[0x0000] = 0x34;			/* ... straddling 0	*/
	gmem[0x0001] = 0x12;
	G.pc = 0xffff;
	z80step(&G, &in);
	chk("an instruction straddling 0xffff", (long)G.rp[P_BC], 0x1234L);
	chk("... and pc wrapped", (long)G.pc, 2L);
}

/* ================================================================== */
/* 4b. the CB group, one class at a time			      */
/* ================================================================== */

/*
 * ONE TEST PER OPCODE CLASS, so that a failure names the class and not
 * "the prefix groups".  The CB group is four classes on one encoding --
 * eight rotates and shifts, then BIT, RES and SET -- and each is
 * checked over EVERY byte operand and, for the forms that read a flag
 * in, both carry inputs, against a reference written here.
 *
 * The reference is deliberately clumsy: it takes the byte apart into
 * eight bits, moves them, and puts it back, which is not how z80exec.c
 * does it.  A reference written as the same shift expression would
 * agree with the implementation by construction and prove nothing --
 * section 3's argument, applied to a second group.
 */
static int rcbrot(op, v, cin, cyout)
int op, v, cin, *cyout;
{
	int in[8], out[8], i, r;

	v &= 0xff;
	for (i = 0; i < 8; i++)
		in[i] = (v >> i) & 1;
	for (i = 0; i < 8; i++)
		out[i] = 0;
	switch (op) {
	case 0:					/* RLC: bit 7 round to 0 */
		for (i = 1; i < 8; i++)
			out[i] = in[i - 1];
		out[0] = in[7];
		*cyout = in[7];
		break;
	case 1:					/* RRC: bit 0 round to 7 */
		for (i = 0; i < 7; i++)
			out[i] = in[i + 1];
		out[7] = in[0];
		*cyout = in[0];
		break;
	case 2:					/* RL: CY into bit 0	*/
		for (i = 1; i < 8; i++)
			out[i] = in[i - 1];
		out[0] = cin;
		*cyout = in[7];
		break;
	case 3:					/* RR: CY into bit 7	*/
		for (i = 0; i < 7; i++)
			out[i] = in[i + 1];
		out[7] = cin;
		*cyout = in[0];
		break;
	case 4:					/* SLA: 0 into bit 0	*/
		for (i = 1; i < 8; i++)
			out[i] = in[i - 1];
		out[0] = 0;
		*cyout = in[7];
		break;
	case 5:					/* SRA: bit 7 held	*/
		for (i = 0; i < 7; i++)
			out[i] = in[i + 1];
		out[7] = in[7];
		*cyout = in[0];
		break;
	case 6:					/* SLL: 1 into bit 0	*/
		for (i = 1; i < 8; i++)
			out[i] = in[i - 1];
		out[0] = 1;
		*cyout = in[7];
		break;
	default:				/* SRL: 0 into bit 7	*/
		for (i = 0; i < 7; i++)
			out[i] = in[i + 1];
		out[7] = 0;
		*cyout = in[0];
		break;
	}
	r = 0;
	for (i = 0; i < 8; i++)
		if (out[i])
			r |= 1 << i;
	return (r);
}

/*
 * Class 1: CB 00-3F, the eight rotates and shifts.
 *
 * They are NOT the base map's four.  0x07 (`RLC A') touches CY alone;
 * CB 07 is the same rotate and sets S, Z and P from the result as well,
 * so a CB group written by calling rot() would be wrong in exactly the
 * bits nothing in an 8080 program reads and everything in a Z80 program
 * does.  Both forms are run here on the same byte and the difference is
 * asserted.
 */
static void t_cb_rot(void)
{
	struct z80in in;
	unsigned char b[2];
	char what[80];
	int op, v, cin, wr, wcy, wf, bad, badr, badm;

	for (op = 0; op < 8; op++) {
		bad = badr = badm = 0;
		for (cin = 0; cin < 2; cin++)
		for (v = 0; v < 256; v++) {
			wcy = 0;
			wr = rcbrot(op, v, cin, &wcy);
			wf = rszp(wr) | (wcy ? F_CY : 0);
			wf &= ~F_Z80X;

			/* the register form, on A */
			gclear();
			b[0] = 0xcb;
			b[1] = (unsigned char)((op << 3) | R_A);
			gcode(b, 2);
			G.a = (z8)v;
			G.f = (z8)(F_ONE | (cin ? F_CY : 0));
			if (z80step(&G, &in) != X_OK || G.pc != 0x102)
				bad++;
			else {
				if ((int)z80flags(&G) != wf)
					bad++;
				if ((int)G.a != wr)
					badr++;
			}

			/* the (HL) form: the same arithmetic, and the
			 * one place r = 6 can be mistaken for a register */
			gclear();
			b[0] = 0xcb;
			b[1] = (unsigned char)((op << 3) | R_M);
			gcode(b, 2);
			G.rp[P_HL] = 0x4000;
			gmem[0x4000] = (char)v;
			G.f = (z8)(F_ONE | (cin ? F_CY : 0));
			if (z80step(&G, &in) != X_OK)
				badm++;
			else {
				if ((int)z80flags(&G) != wf)
					badm++;
				if ((gmem[0x4000] & 0xff) != wr)
					badm++;
			}
		}
		sprintf(what, "cb rot %d (register)", op);
		chk(what, bad, 0);
		sprintf(what, "cb rot %d result", op);
		chk(what, badr, 0);
		sprintf(what, "cb rot %d (hl)", op);
		chk(what, badm, 0);
	}

	/* every one of the eight register slots, on one operation, so
	 * that a swapped .y field is caught by name */
	for (op = 0; op < 8; op++) {
		if (op == R_M)
			continue;
		gclear();
		b[0] = 0xcb;
		b[1] = (unsigned char)(0x00 | op);	/* rlc r	*/
		gcode(b, 2);
		z80setr(&G, op, 0x81);
		sprintf(what, "cb rlc reaches register %d", op);
		chk(what, z80step(&G, &in), X_OK);
		chk(what, z80getr(&G, op), 0x03);
		chk(what, (int)(z80flags(&G) & F_CY), F_CY);
	}

	/* 0x07 and CB 07 are the same rotate and NOT the same flags:
	 * the base form leaves S, Z and P alone, the CB form sets them.
	 * 0x80 rotates to 0x01, which is neither zero nor even parity,
	 * so a Z and P left set by something earlier discriminate. */
	gclear();
	b[0] = 0x07;
	gcode(b, 1);
	G.a = 0x80;
	G.f = (z8)(F_ONE | F_ZE | F_PA);
	z80step(&G, &in);
	chk("0x07 rlc a leaves Z alone", (int)(z80flags(&G) & F_ZE), F_ZE);
	gclear();
	b[0] = 0xcb;
	b[1] = 0x07;
	gcode(b, 2);
	G.a = 0x80;
	G.f = (z8)(F_ONE | F_ZE | F_PA);
	z80step(&G, &in);
	chk("cb 07 rlc a clears Z", (int)(z80flags(&G) & F_ZE), 0);
	chk("... same result byte", (int)G.a, 0x01);
}

/*
 * Class 2: CB 40-7F, BIT b,r.
 *
 * Four claims, and the third is the one a summary gets wrong: Z is the
 * COMPLEMENT of the bit, CY is PRESERVED, P/V is a copy of Z (not the
 * parity of anything), and S is set only when the bit tested is bit 7
 * and it is set.  H is set unconditionally.
 */
static void t_cb_bit(void)
{
	struct z80in in;
	unsigned char b[2];
	char what[80];
	int bit, v, cin, set, wf, bad;

	for (bit = 0; bit < 8; bit++) {
		bad = 0;
		for (cin = 0; cin < 2; cin++)
		for (v = 0; v < 256; v++) {
			set = (v >> bit) & 1;
			wf = F_ONE | F_AC;
			if (!set)
				wf |= F_ZE | F_PA;	/* P/V = Z	*/
			if (set && bit == 7)
				wf |= F_SI;
			if (cin)
				wf |= F_CY;		/* PRESERVED	*/

			gclear();
			b[0] = 0xcb;
			b[1] = (unsigned char)(0x40 | (bit << 3) | R_A);
			gcode(b, 2);
			G.a = (z8)v;
			G.f = (z8)(F_ONE | (cin ? F_CY : 0));
			if (z80step(&G, &in) != X_OK)
				bad++;
			else {
				if ((int)z80flags(&G) != wf)
					bad++;
				if ((int)G.a != v)  /* BIT stores nothing */
					bad++;
			}

			gclear();
			b[0] = 0xcb;
			b[1] = (unsigned char)(0x40 | (bit << 3) | R_M);
			gcode(b, 2);
			G.rp[P_HL] = 0x4000;
			gmem[0x4000] = (char)v;
			G.f = (z8)(F_ONE | (cin ? F_CY : 0));
			if (z80step(&G, &in) != X_OK)
				bad++;
			else if ((int)z80flags(&G) != wf)
				bad++;
			else if ((gmem[0x4000] & 0xff) != v)
				bad++;
		}
		sprintf(what, "cb bit %d", bit);
		chk(what, bad, 0);
	}
}

/*
 * Classes 3 and 4: CB 80-BF (RES) and CB C0-FF (SET).
 *
 * They set NO flags at all, which is the claim worth testing: a pending
 * lazy record must survive a RES between the ADD that made it and the
 * JZ that reads it.  So the RES half runs an ADD first and asserts the
 * ADD's flags are still there afterwards.
 */
static void t_cb_setres(void)
{
	struct z80in in;
	unsigned char b[4];
	char what[80];
	int bit, v, w, bad;

	for (bit = 0; bit < 8; bit++) {
		bad = 0;
		for (v = 0; v < 256; v++) {
			/* RES b,B, with an ADD A,A pending */
			gclear();
			b[0] = 0x87;			/* add a,a	*/
			b[1] = 0xcb;
			b[2] = (unsigned char)(0x80 | (bit << 3) | R_B);
			gcode(b, 3);
			G.a = 0x80;			/* -> 0, Z P CY	*/
			z80setr(&G, R_B, v);
			z80step(&G, &in);
			if (z80step(&G, &in) != X_OK)
				bad++;
			w = v & ~(1 << bit);
			if (z80getr(&G, R_B) != w)
				bad++;
			if ((int)z80flags(&G)
			    != (F_ONE | F_ZE | F_PA | F_CY))
				bad++;

			/* SET b,(HL) */
			gclear();
			b[0] = 0xcb;
			b[1] = (unsigned char)(0xc0 | (bit << 3) | R_M);
			gcode(b, 2);
			G.rp[P_HL] = 0x4000;
			gmem[0x4000] = (char)v;
			G.f = (z8)(F_ONE | F_CY);
			if (z80step(&G, &in) != X_OK)
				bad++;
			if ((gmem[0x4000] & 0xff) != (v | (1 << bit)))
				bad++;
			if ((int)z80flags(&G) != (F_ONE | F_CY))
				bad++;
		}
		sprintf(what, "cb res/set %d", bit);
		chk(what, bad, 0);
	}
}

/* ================================================================== */
/* 4c. the ED group, one class at a time			      */
/* ================================================================== */

/*
 * Class 5: LDI, LDD, LDIR, LDDR.
 *
 * Three claims.  The transfer itself and the three pointer updates;
 * P/V = "BC is still non-zero", which is the flag a hand-written copy
 * loop branches on after LDI; and the one that a block move written
 * carelessly gets wrong -- S, Z and CY are UNTOUCHED, so a compare
 * before the move is still readable after it.
 */
static void t_ed_ldblk(void)
{
	struct z80in in;
	unsigned char b[2];
	int i, bad;

	/* ---- LDI: one byte up, and the three pointers ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xa0;
	gcode(b, 2);
	G.rp[P_HL] = 0x4000;
	G.rp[P_DE] = 0x5000;
	G.rp[P_BC] = 3;
	gmem[0x4000] = (char)0x5a;
	G.f = (z8)(F_ONE | F_SI | F_ZE | F_CY);
	chk("ldi runs", z80step(&G, &in), X_OK);
	chk("ldi copies the byte", gmem[0x5000] & 0xff, 0x5a);
	chk("ldi bumps hl", (long)G.rp[P_HL], 0x4001L);
	chk("ldi bumps de", (long)G.rp[P_DE], 0x5001L);
	chk("ldi drops bc", (long)G.rp[P_BC], 2L);
	chk("ldi sets P/V while bc is non-zero",
		(int)z80flags(&G), F_ONE | F_SI | F_ZE | F_CY | F_PA);

	/* ---- the last byte of a run clears P/V ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xa0;
	gcode(b, 2);
	G.rp[P_HL] = 0x4000;
	G.rp[P_DE] = 0x5000;
	G.rp[P_BC] = 1;
	G.f = (z8)(F_ONE | F_CY);
	z80step(&G, &in);
	chk("ldi clears P/V when bc reaches zero",
		(int)z80flags(&G), F_ONE | F_CY);

	/* ---- LDD: the same, downwards ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xa8;
	gcode(b, 2);
	G.rp[P_HL] = 0x4000;
	G.rp[P_DE] = 0x5000;
	G.rp[P_BC] = 3;
	gmem[0x4000] = (char)0x5a;
	chk("ldd runs", z80step(&G, &in), X_OK);
	chk("ldd copies the byte", gmem[0x5000] & 0xff, 0x5a);
	chk("ldd drops hl", (long)G.rp[P_HL], 0x3fffL);
	chk("ldd drops de", (long)G.rp[P_DE], 0x4fffL);
	chk("ldd drops bc", (long)G.rp[P_BC], 2L);

	/* ---- LDIR: 256 bytes, and the repeat is PC going back two
	 * bytes, so it costs 256 instructions and not one ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xb0;
	gcode(b, 2);
	G.rp[P_HL] = 0x4000;
	G.rp[P_DE] = 0x5000;
	G.rp[P_BC] = 256;
	for (i = 0; i < 256; i++)
		gmem[0x4000 + i] = (char)(i ^ 0x33);
	G.f = (z8)(F_ONE | F_SI | F_ZE | F_CY);
	for (i = 0; i < 300 && G.pc == 0x100; i++)
		if (z80step(&G, &in) != X_OK)
			break;
	chk("ldir takes one instruction per byte", (long)i, 256L);
	chk("ldir ends past the instruction", (long)G.pc, 0x102L);
	bad = 0;
	for (i = 0; i < 256; i++)
		if ((gmem[0x5000 + i] & 0xff) != ((i ^ 0x33) & 0xff))
			bad++;
	chk("ldir copied every byte", bad, 0);
	chk("ldir left bc zero", (long)G.rp[P_BC], 0L);
	chk("ldir left hl past the source", (long)G.rp[P_HL], 0x4100L);
	chk("ldir left de past the destination", (long)G.rp[P_DE], 0x5100L);
	chk("ldir preserved S, Z and CY and cleared P/V",
		(int)z80flags(&G), F_ONE | F_SI | F_ZE | F_CY);

	/* ---- LDDR: the same block, backwards, and the answer is the
	 * same block: this is the direction an overlapping move
	 * upwards has to use ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xb8;
	gcode(b, 2);
	G.rp[P_HL] = 0x40ff;
	G.rp[P_DE] = 0x50ff;
	G.rp[P_BC] = 256;
	for (i = 0; i < 256; i++)
		gmem[0x4000 + i] = (char)(i ^ 0x5c);
	for (i = 0; i < 300 && G.pc == 0x100; i++)
		if (z80step(&G, &in) != X_OK)
			break;
	chk("lddr takes one instruction per byte", (long)i, 256L);
	bad = 0;
	for (i = 0; i < 256; i++)
		if ((gmem[0x5000 + i] & 0xff) != ((i ^ 0x5c) & 0xff))
			bad++;
	chk("lddr copied every byte", bad, 0);
	chk("lddr left hl below the source", (long)G.rp[P_HL], 0x3fffL);

	/* ---- the addresses wrap at 64 KB, which is the memory model
	 * the whole shim rests on and not a special case here ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xa0;
	gcode(b, 2);
	G.rp[P_HL] = 0xffff;
	G.rp[P_DE] = 0xffff;
	G.rp[P_BC] = 2;
	gmem[0xffff] = (char)0x77;
	z80step(&G, &in);
	chk("ldi wraps hl at 64 KB", (long)G.rp[P_HL], 0L);
	chk("ldi wraps de at 64 KB", (long)G.rp[P_DE], 0L);
}

/*
 * Class 6: CPI, CPD, CPIR, CPDR.
 *
 * The compare's own flags are checked DIFFERENTIALLY against section
 * 3's 8080 subtract reference over all 65,536 operand pairs, with the
 * two bits that are not a subtract's asserted separately: CY is
 * PRESERVED (a CP would set it) and P/V is the counter and not parity.
 * Then the repeat, whose whole subtlety is that it stops on EITHER a
 * match or exhaustion.
 */
static void t_ed_cpblk(void)
{
	struct z80in in;
	unsigned char b[2];
	int a, v, cin, wf, wr, bad, i;

	bad = 0;
	for (cin = 0; cin < 2; cin++)
	for (a = 0; a < 256; a++)
	for (v = 0; v < 256; v++) {
		wf = ralu(2, a, v, 0, &wr);	/* the 8080 SUB reference */
		wf &= ~(F_CY | F_PA);		/* CY preserved, P/V is BC */
		wf |= F_PA;			/* BC = 2 below, so non-zero */
		if (cin)
			wf |= F_CY;

		gclear();
		b[0] = 0xed; b[1] = 0xa1;
		gcode(b, 2);
		G.a = (z8)a;
		G.rp[P_HL] = 0x4000;
		G.rp[P_BC] = 3;
		gmem[0x4000] = (char)v;
		G.f = (z8)(F_ONE | (cin ? F_CY : 0));
		if (z80step(&G, &in) != X_OK)
			bad++;
		else {
			if ((int)z80flags(&G) != wf)
				bad++;
			if ((int)G.a != a)	/* a compare stores nothing */
				bad++;
			if (G.rp[P_HL] != 0x4001 || G.rp[P_BC] != 2)
				bad++;
		}
	}
	chk("cpi flags against the subtract reference", bad, 0);

	/* ---- the last compare of a run clears P/V even on a match ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xa1;
	gcode(b, 2);
	G.a = 0x41;
	G.rp[P_HL] = 0x4000;
	G.rp[P_BC] = 1;
	gmem[0x4000] = (char)0x41;
	z80step(&G, &in);
	chk("cpi on a match sets Z", (int)(z80flags(&G) & F_ZE), F_ZE);
	chk("cpi with bc exhausted clears P/V",
		(int)(z80flags(&G) & F_PA), 0);

	/* ---- CPD walks down ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xa9;
	gcode(b, 2);
	G.a = 0x10;
	G.rp[P_HL] = 0x4000;
	G.rp[P_BC] = 4;
	chk("cpd runs", z80step(&G, &in), X_OK);
	chk("cpd drops hl", (long)G.rp[P_HL], 0x3fffL);
	chk("cpd drops bc", (long)G.rp[P_BC], 3L);

	/* ---- CPIR finds the byte: it stops with HL PAST the match,
	 * which is the convention every CP/M string routine relies on --- */
	gclear();
	b[0] = 0xed; b[1] = 0xb1;
	gcode(b, 2);
	G.a = 0x14;
	G.rp[P_HL] = 0x4000;
	G.rp[P_BC] = 32;
	for (i = 0; i < 32; i++)
		gmem[0x4000 + i] = (char)i;
	for (i = 0; i < 64 && G.pc == 0x100; i++)
		if (z80step(&G, &in) != X_OK)
			break;
	chk("cpir stops at the match", (long)i, 0x15L);
	chk("cpir leaves hl past the match", (long)G.rp[P_HL], 0x4015L);
	chk("cpir leaves bc counting what is left",
		(long)G.rp[P_BC], (long)(32 - 0x15));
	chk("cpir sets Z on the match", (int)(z80flags(&G) & F_ZE), F_ZE);
	chk("cpir leaves P/V set: the block is not exhausted",
		(int)(z80flags(&G) & F_PA), F_PA);

	/* ---- CPIR that finds nothing runs the block out ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xb1;
	gcode(b, 2);
	G.a = 0xee;
	G.rp[P_HL] = 0x4000;
	G.rp[P_BC] = 32;
	for (i = 0; i < 32; i++)
		gmem[0x4000 + i] = (char)i;
	for (i = 0; i < 64 && G.pc == 0x100; i++)
		if (z80step(&G, &in) != X_OK)
			break;
	chk("cpir with no match runs the block out", (long)i, 32L);
	chk("... bc reaches zero", (long)G.rp[P_BC], 0L);
	chk("... Z is clear", (int)(z80flags(&G) & F_ZE), 0);
	chk("... and P/V says the block is exhausted",
		(int)(z80flags(&G) & F_PA), 0);

	/* ---- CPDR, downwards ---- */
	gclear();
	b[0] = 0xed; b[1] = 0xb9;
	gcode(b, 2);
	G.a = 0x02;
	G.rp[P_HL] = 0x401f;
	G.rp[P_BC] = 32;
	for (i = 0; i < 32; i++)
		gmem[0x4000 + i] = (char)i;
	for (i = 0; i < 64 && G.pc == 0x100; i++)
		if (z80step(&G, &in) != X_OK)
			break;
	chk("cpdr stops at the match", (long)G.rp[P_HL], 0x4001L);
	chk("cpdr sets Z", (int)(z80flags(&G) & F_ZE), F_ZE);
}

/*
 * Class 7: ADC HL,ss and SBC HL,ss -- and with them the eight
 * LD (nn),dd / LD dd,(nn) forms and NEG, which are the rest of what
 * this stage takes out of the ED space.
 *
 * The 16-bit reference is written in LONG arithmetic with the signed
 * range tested explicitly, because P/V here is OVERFLOW and not parity:
 * these encodings have no 8080 form, so there is no 8080 rule to
 * inherit.  (NEG does share a form -- it is a subtract, and it gets the
 * file's parity like every other subtract.)
 */
static long r16(sub, hl, ss, cin, wf)
int sub;
long hl, ss;
int cin;
int *wf;
{
	long res, sh, sl, sr;
	int f;

	sh = (hl >= 0x8000L) ? hl - 0x10000L : hl;
	sl = (ss >= 0x8000L) ? ss - 0x10000L : ss;
	f = F_ONE;
	if (sub) {				/* ADC HL,ss		*/
		res = hl + ss + cin;
		sr = sh + sl + cin;
		if (res > 0xffffL)
			f |= F_CY;
		if (((hl & 0xfffL) + (ss & 0xfffL) + cin) > 0xfffL)
			f |= F_AC;
	} else {				/* SBC HL,ss		*/
		res = hl - ss - cin;
		sr = sh - sl - cin;
		if (res < 0)
			f |= F_CY;
		/* this file's AC convention: SET when there was NO
		 * half borrow, exactly as after any other subtract */
		if (((hl & 0xfffL) - (ss & 0xfffL) - cin) >= 0)
			f |= F_AC;
	}
	res &= 0xffffL;
	if (res == 0)
		f |= F_ZE;
	if (res & 0x8000L)
		f |= F_SI;
	if (sr > 32767L || sr < -32768L)
		f |= F_PA;			/* P/V is OVERFLOW	*/
	*wf = f;
	return (res);
}

static void t_ed_arith(void)
{
	struct z80in in;
	unsigned char b[4];
	static long vals[14] = {
		0x0000L, 0x0001L, 0x000fL, 0x0fffL, 0x1000L, 0x7fffL,
		0x8000L, 0x8001L, 0xffffL, 0xfffeL, 0x1234L, 0xedcbL,
		0x00ffL, 0x0100L
	};
	char what[80];
	long wr;
	int i, j, cin, add, rp, wf, bad;

	for (add = 0; add < 2; add++) {
		bad = 0;
		for (cin = 0; cin < 2; cin++)
		for (i = 0; i < 14; i++)
		for (j = 0; j < 14; j++) {
			wr = r16(add, vals[i], vals[j], cin, &wf);

			gclear();
			b[0] = 0xed;
			b[1] = (unsigned char)(add ? 0x5a : 0x52);
			gcode(b, 2);			/* ss = DE	*/
			G.rp[P_HL] = (z16)vals[i];
			G.rp[P_DE] = (z16)vals[j];
			G.f = (z8)(F_ONE | (cin ? F_CY : 0));
			if (z80step(&G, &in) != X_OK)
				bad++;
			else {
				if ((long)G.rp[P_HL] != wr)
					bad++;
				if ((int)z80flags(&G) != wf)
					bad++;
			}
		}
		sprintf(what, "%s hl,ss over the reference",
			add ? "adc" : "sbc");
		chk(what, bad, 0);
	}

	/* every pair the field can name, including SP, and HL against
	 * itself -- which is how a Z80 doubles HL with a carry out */
	for (rp = 0; rp < 4; rp++) {
		gclear();
		b[0] = 0xed;
		b[1] = (unsigned char)(0x4a | (rp << 4));   /* adc hl,rp */
		gcode(b, 2);
		G.rp[P_HL] = 0x1000;
		G.rp[rp] = 0x0234;		/* rp = HL overwrites HL */
		sprintf(what, "adc hl,rp %d", rp);
		chk(what, z80step(&G, &in), X_OK);
		/* HL against ITSELF is how a Z80 doubles it, so that
		 * case is 0x234 + 0x234 and not 0x1000 + anything */
		chk(what, (long)G.rp[P_HL],
			rp == P_HL ? 0x0468L : 0x1234L);
	}

	/* ---- the eight LD (nn),dd and LD dd,(nn): four bytes, little
	 * endian, and no flag touched at all ---- */
	for (rp = 0; rp < 4; rp++) {
		gclear();
		b[0] = 0xed;
		b[1] = (unsigned char)(0x43 | (rp << 4));   /* ld (nn),rp */
		b[2] = 0x00;
		b[3] = 0x40;
		gcode(b, 4);
		G.rp[rp] = 0xbeef;
		G.f = (z8)(F_ONE | F_CY | F_ZE);
		sprintf(what, "ld (nn),rp %d", rp);
		chk(what, z80step(&G, &in), X_OK);
		chk(what, (long)G.pc, 0x104L);
		chk(what, gmem[0x4000] & 0xff, 0xef);
		chk(what, gmem[0x4001] & 0xff, 0xbe);
		chk(what, (int)z80flags(&G), F_ONE | F_CY | F_ZE);

		gclear();
		b[0] = 0xed;
		b[1] = (unsigned char)(0x4b | (rp << 4));   /* ld rp,(nn) */
		b[2] = 0x00;
		b[3] = 0x40;
		gcode(b, 4);
		gmem[0x4000] = (char)0xcd;
		gmem[0x4001] = (char)0xab;
		sprintf(what, "ld rp %d,(nn)", rp);
		chk(what, z80step(&G, &in), X_OK);
		chk(what, (long)G.rp[rp], 0xabcdL);
	}

	/* ---- NEG, over every accumulator, against the SUB reference,
	 * and in all eight of its encodings ---- */
	bad = 0;
	for (i = 0; i < 256; i++) {
		int wres;

		wf = ralu(2, 0, i, 0, &wres);
		gclear();
		b[0] = 0xed; b[1] = 0x44;
		gcode(b, 2);
		G.a = (z8)i;
		if (z80step(&G, &in) != X_OK)
			bad++;
		else {
			if ((int)G.a != wres)
				bad++;
			if ((int)z80flags(&G) != wf)
				bad++;
		}
	}
	chk("neg against the subtract reference", bad, 0);
	bad = 0;
	for (i = 0; i < 8; i++) {
		gclear();
		b[0] = 0xed;
		b[1] = (unsigned char)(0x44 | (i << 3));
		gcode(b, 2);
		G.a = 0x01;
		if (z80step(&G, &in) != X_OK || G.a != 0xff)
			bad++;
	}
	chk("all eight neg encodings", bad, 0);
}

/*
 * 4d. THE MEASUREMENT: how often the new classes materialise.
 *
 * The point of the lazy scheme is that a flag is computed when it is
 * READ, not when it is written, and a CB/ED implementation that is
 * perfectly correct can still destroy that property without any of the
 * tests above noticing.  So it is asserted, as a number, per class:
 * a 256-byte LDIR is 256 instructions and must cost ONE materialisation
 * -- not 256 -- because every iteration after the first finds a pending
 * record of its own class whose preserved bits are still current.
 *
 * The printed line is the deliverable; the chk()s below it are what
 * stops the number drifting.
 */
static void t_lazyrate(void)
{
	struct z80in in;
	unsigned char b[2];
	z32 nldir, ncpir, nrot, nbit;
	int i;

	/* LDIR, 256 bytes */
	gclear();
	b[0] = 0xed; b[1] = 0xb0;
	gcode(b, 2);
	G.rp[P_HL] = 0x4000;
	G.rp[P_DE] = 0x5000;
	G.rp[P_BC] = 256;
	z80ninsn = z80nflag = 0;
	for (i = 0; i < 300 && G.pc == 0x100; i++)
		z80step(&G, &in);
	z80flags(&G);				/* one reader at the end */
	nldir = z80nflag;

	/* CPIR, 256 bytes, no match */
	gclear();
	b[0] = 0xed; b[1] = 0xb1;
	gcode(b, 2);
	G.a = 0xff;
	G.rp[P_HL] = 0x4000;
	G.rp[P_BC] = 256;
	z80nflag = 0;
	for (i = 0; i < 300 && G.pc == 0x100; i++)
		z80step(&G, &in);
	z80flags(&G);
	ncpir = z80nflag;

	/* 256 CB shifts in a row, one reader at the end.  SRL does not
	 * read the carry in, so nothing in the run consumes a flag. */
	gclear();
	for (i = 0; i < 256; i++) {
		gmem[0x100 + 2 * i] = (char)0xcb;
		gmem[0x100 + 2 * i + 1] = (char)0x3f;	/* srl a	*/
	}
	G.pc = 0x100;
	G.a = 0xff;
	z80nflag = 0;
	for (i = 0; i < 256; i++)
		z80step(&G, &in);
	z80flags(&G);
	nrot = z80nflag;

	/* 256 BITs in a row */
	gclear();
	for (i = 0; i < 256; i++) {
		gmem[0x100 + 2 * i] = (char)0xcb;
		gmem[0x100 + 2 * i + 1] = (char)0x7f;	/* bit 7,a	*/
	}
	G.pc = 0x100;
	G.a = 0x80;
	z80nflag = 0;
	for (i = 0; i < 256; i++)
		z80step(&G, &in);
	z80flags(&G);
	nbit = z80nflag;

	printf("z80test: lazy flags over 256 instructions of one class: "
		"ldir %lu, cpir %lu, srl %lu, bit %lu "
		"(eager would be 256 each)\n",
		(unsigned long)nldir, (unsigned long)ncpir,
		(unsigned long)nrot, (unsigned long)nbit);
	chk("ldir materialises once, not once per byte", (long)nldir, 1L);
	chk("cpir materialises once, not once per byte", (long)ncpir, 1L);
	chk("a run of srl materialises once", (long)nrot, 1L);
	chk("a run of bit materialises once", (long)nbit, 1L);
}

/*
 * 4e. Delay loops.  DCR r; JNZ back (or JR NZ back), run with the fast
 * path and without it, must end in the same machine, memory and counts.
 * A leading ADI leaves a pending record for the first DCR to fold.
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
	struct z80in in;
	long k;

	for (k = 0; k < 2000; k++)
		if (z80step(&G, &in) != X_OK)
			break;
	return (k);
}

static int dload(int x, int jr, int pre, int v, int cin)
{
	static const int regs[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0, 0x77 };
	int i, p;

	gclear();
	for (i = 0; i < 0x10000; i += 7)
		gmem[i] = (char)(i * 13);
	p = 0x100;
	if (pre) {
		gmem[p++] = (char)0xc6;		/* adi 80h		*/
		gmem[p++] = (char)0x80;
	}
	gmem[p] = (char)(0x05 | (x << 3));	/* dcr r		*/
	if (jr) {
		gmem[p + 1] = (char)0x20;	/* jr nz,$-1		*/
		gmem[p + 2] = (char)0xfd;
		gmem[p + 3] = (char)0x76;	/* hlt			*/
	} else {
		gmem[p + 1] = (char)0xc2;	/* jnz p		*/
		gmem[p + 2] = (char)(p & 0xff);
		gmem[p + 3] = (char)(p >> 8);
		gmem[p + 4] = (char)0x76;
	}
	G.pc = 0x100;
	for (i = 0; i < 8; i++)
		if (i != R_M)
			z80setr(&G, i, regs[i]);
	G.rp[P_SP] = 0xfff0;
	z80setr(&G, x, v);
	if (cin)
		G.f |= F_CY;
	return (0);
}

static void t_delay(void)
{
	static const int vals[] = { 0, 1, 2, 3, 127, 128, 254, 255 };
	static char ref[0x10000];
	struct z80 want;
	z32 wi, wf;
	long ws, gs;
	int x, jr, pre, cin, vi, bad, ncase;

	bad = ncase = 0;
	for (x = 0; x < 8; x++)
	for (jr = 0; jr < 2; jr++)
	for (pre = 0; pre < 2; pre++)
	for (cin = 0; cin < 2; cin++)
	for (vi = 0; vi < (int)(sizeof vals / sizeof vals[0]); vi++) {
		if (x == R_M)
			continue;
		ncase++;
		dload(x, jr, pre, vals[vi], cin);
		z80fast = 0;
		z80ninsn = z80nflag = z80nskip = 0;
		ws = drun();
		wi = z80ninsn;
		wf = z80nflag;
		want = G;
		memcpy(ref, gmem, sizeof ref);

		dload(x, jr, pre, vals[vi], cin);
		z80fast = 1;
		z80ninsn = z80nflag = z80nskip = 0;
		gs = drun();
		if (memcmp(&G, &want, sizeof G) != 0
		 || memcmp(gmem, ref, sizeof ref) != 0
		 || z80ninsn != wi || z80nflag != wf
		 || gs + (long)z80nskip != ws) {
			if (bad++ < 4)
				printf("FAIL delay r%d jr%d pre%d cy%d v%d: "
					"insn %lu/%lu flag %lu/%lu steps "
					"%ld+%lu/%ld pc %04x/%04x f %02x/%02x\n",
					x, jr, pre, cin, vals[vi],
					(unsigned long)z80ninsn,
					(unsigned long)wi,
					(unsigned long)z80nflag,
					(unsigned long)wf, gs,
					(unsigned long)z80nskip, ws,
					G.pc, want.pc, G.f, want.f);
		}
	}
	ntest++;
	if (bad) {
		printf("FAIL delay loops: %d of %d cases differ\n", bad, ncase);
		nfail++;
	}
	chk("delay loops compared", (long)ncase, 7L * 2 * 2 * 2 * 8);

	/* The time.  256 turns of JNZ are 3584 T-states and of JR 4091;
	 * a tick is 40000.  Each sleep asks one tick more than it owes. */
	z80wait = dwait;
	for (vi = 0; vi < 12; vi++) {
		dload(R_A, 0, 0, 0, 0);
		drun();
		if (vi == 10)
			chk("delay: no sleep below a tick", (long)dwaits, 0L);
	}
	chk("delay: 43008 T-states sleep once", (long)dwaits, 1L);
	chk("delay: ... asking two ticks", (long)dlast, 2L);
	for (vi = 0; vi < 10; vi++) {
		dload(R_B, 1, 0, 0, 0);
		drun();
		if (vi == 8)
			chk("delay: the 3008 left carries", (long)dwaits, 1L);
	}
	chk("delay: 3008 + 40910 sleeps again", (long)dwaits, 2L);
	z80wait = 0;
	z80nskip = 0;
}

/*
 * z80lcond() against z80cond(z80flags()) for every class, every
 * condition and every operand pair, carry-in and preserved CY.  The
 * record must come back untouched or materialised, so a later PUSH PSW
 * still sees the same flags.
 */
static long lcbad[11];

static void lcone(int cls, int a, int b, int r, int c, int f)
{
	struct z80 m, m2, mf;
	int cc, want;

	memset(&m, 0, sizeof m);
	m.lz = (z8)cls;
	m.la = (z16)(a & 0xff);
	m.lb = (z16)(b & 0xff);
	m.lr = (z16)(r & 0xff);
	m.lc = (z8)c;
	m.f = (z8)((f | F_ONE) & ~F_Z80X);
	for (cc = 0; cc < 8; cc++) {
		mf = m;
		want = z80cond((int)z80flags(&mf), cc);
		m2 = m;
		if (z80lcond(&m2, cc) != want
		 || (memcmp(&m2, &m, sizeof m) != 0
		  && memcmp(&m2, &mf, sizeof m) != 0)) {
			if (lcbad[cls]++ == 0)
				printf("z80test: lcond class %d cc %d a %02x "
					"b %02x r %02x c %d f %02x\n",
					cls, cc, m.la, m.lb, m.lr, c, m.f);
		}
	}
}

static void t_lazycond(void)
{
	int a, b, c, f;
	z32 n0;

	n0 = z80nflag;
	for (f = 0; f < 256; f++) {
		lcone(LZ_NONE, 0, 0, 0, 0, f);
		lcone(LZ_LDBLK, 0, 0, 0, f & 1, f);
	}
	for (a = 0; a < 256; a++)
		for (b = 0; b < 256; b++) {
			for (c = 0; c < 2; c++) {
				lcone(LZ_ADD, a, b, a + b + c, c, 0);
				lcone(LZ_SUB, a, b, a - b - c, c, 0);
				lcone(LZ_CPBLK, a, b, a - b, c, b);
				lcone(LZ_ROT, a, 0, b, c, 0);
			}
			lcone(LZ_AND, a, b, a & b, 0, 0);
			lcone(LZ_LOG, a, b, a | b, 0, 0);
			lcone(LZ_LOG, a, b, a ^ b, 0, 0);
			/* b stands in for the flags the class preserves */
			lcone(LZ_INR, a, 1, a + 1, 0, b);
			lcone(LZ_DCR, a, 1, a - 1, 0, b);
			if (b < 8)
				for (f = 0; f < 256; f++)
					lcone(LZ_BIT, a, b, a & (1 << b), 0, f);
		}
	z80nflag = n0;
	for (c = 0; c <= LZ_CPBLK; c++)
		chk("lazy condition matches the materialised one",
			lcbad[c], 0L);
}

/* ================================================================== */
/* 5. refusals							      */
/* ================================================================== */

/*
 * Everything stage one decodes and does not execute must return
 * X_UNIMP with PC UNMOVED, so that the caller can name the address
 * that could not run.  An honest refusal beats a silent wrong answer,
 * and this section is the check that the refusal is honest.
 */
static void t_refuse(void)
{
	struct z80in in;
	static unsigned char rows[8][4] = {
		{0xed, 0x46, 0, 0},		/* im 0			*/
		{0xed, 0x67, 0, 0},		/* rrd			*/
		{0xdd, 0x21, 0x34, 0x12},	/* ld ix,nn		*/
		{0xfd, 0x7e, 0x05, 0},		/* ld a,(iy+5)		*/
		{0xdd, 0xcb, 0x05, 0x06},	/* rlc (ix+5)		*/
		{0xdb, 0x10, 0, 0},		/* in a,(10h)		*/
		{0xd3, 0x10, 0, 0},		/* out (10h),a		*/
		{0x76, 0, 0, 0}			/* hlt			*/
	};
	char what[80];
	int i, r;

	for (i = 0; i < 8; i++) {
		gclear();
		gcode(rows[i], 4);
		r = z80step(&G, &in);
		sprintf(what, "refuse[%02x %02x]", rows[i][0], rows[i][1]);
		chk(what, r, i == 7 ? X_HALT : X_UNIMP);
		chk(what, (long)G.pc, 0x100L);
	}

	/* And the one thing that is genuinely not an instruction. */
	gclear();
	{
		unsigned char b[3];

		b[0] = 0xed; b[1] = 0xfe; b[2] = (unsigned char)(HOOK_MAX + 3);
		gcode(b, 3);
	}
	chk("a hook we never planted is X_BAD", z80step(&G, &in), X_BAD);
	chk("... with pc unmoved", (long)G.pc, 0x100L);
}

/* ================================================================== */
/* 6. the loader						      */
/* ================================================================== */

static void t_loader(void)
{
	static char img[0x10000];
	struct comrsx r;
	int rc, i, bad;

	/* every refusal */
	chk("empty file", z80load(&G, gmem, img, 0L), CL_EMPTY);

	/* A header record with no descriptor in it declares no RSX at all,
	 * and there is then nothing left to refuse. */
	img[0] = (char)0xc9;
	chk("a GENCOM header with no descriptors loads", z80load(&G, gmem,
		img, (long)RSX_HDRLEN + 16), CL_OK);
	chk("... and places no module", z80nrsx, 0);
	chk("... and leaves the TPA where it was", (long)z80rsxtop,
		(long)GUESTTOP);

	/* A descriptor whose image and bitmap run past the end of the file
	 * is the one thing left that CL_RSX names. */
	img[0x10] = 0x00;
	img[0x11] = 0x02;		/* offset 0x0200			*/
	img[0x12] = 0x00;
	img[0x13] = 0x01;		/* length 0x0100, bitmap to 0x0320	*/
	chk("a descriptor past the end of the file is refused",
		z80load(&G, gmem, img, 0x300L), CL_RSX);
	img[0x10] = img[0x11] = img[0x12] = img[0x13] = 0;

	img[0] = 0x00;
	chk("an all-zero file is refused", z80load(&G, gmem, img, 128L),
		CL_NOTCOM);
	img[0] = (char)0xff;
	chk("an erased file is refused", z80load(&G, gmem, img, 128L),
		CL_NOTCOM);

	img[0] = (char)0xc3;
	chk("an image over the ceiling is refused",
		z80load(&G, gmem, img, (long)(GUESTTOP - COM_ORG) + 1),
		CL_BIG);
	chk("... and one exactly at it is not",
		z80load(&G, gmem, img, (long)(GUESTTOP - COM_ORG)), CL_OK);

	/* every refusal has a sentence, and no two share one */
	bad = 0;
	z80rsxwho[0] = 'X';
	z80rsxwho[1] = '\0';
	for (i = CL_OK; i <= CL_RSXFIT; i++) {
		int j;

		if (z80lerr(i)[0] == '\0')
			bad++;
		for (j = CL_OK; j < i; j++)
			if (strcmp(z80lerr(i), z80lerr(j)) == 0)
				bad++;
	}
	chk("every CL_ code has its own sentence", bad, 0);

	/* a real load: page zero, the furniture, the registers */
	img[0] = (char)0xc3;
	img[1] = 0x00;
	img[2] = 0x02;
	rc = z80load(&G, gmem, img, 3L);
	chk("load", rc, CL_OK);
	chk("pc is 0x100", (long)G.pc, 0x100L);
	chk("image placed at 0x100", (long)(gmem[0x100] & 0xff), 0xc3L);
	chk("flag bit 1 is set at entry", (long)(G.f & F_ONE), (long)F_ONE);

	chk("warm boot vector", (long)(gmem[0] & 0xff), 0xc3L);
	chk("warm boot target is BIOS entry 1",
		(long)(((gmem[2] & 0xff) << 8) | (gmem[1] & 0xff)),
		(long)(FAKEBIOS + 3));
	chk("BDOS vector", (long)(gmem[5] & 0xff), 0xc3L);
	chk("BDOS entry is six above the TPA top",
		(long)(((gmem[7] & 0xff) << 8) | (gmem[6] & 0xff)),
		(long)(GUESTTOP + 6));
	chk("the BDOS entry is a hook", (long)(gmem[FAKEBDOS] & 0xff), 0xedL);
	chk("... hook 0", (long)(gmem[FAKEBDOS + 2] & 0xff),
		(long)HOOK_BDOS);
	chk("... followed by a RET", (long)(gmem[FAKEBDOS + 3] & 0xff),
		0xc9L);

	/* the BIOS table, entry by entry: table index k is hook
	 * HOOK_BIOS + k, and getting that off by one calls CONIN where
	 * the guest asked for CONST */
	bad = 0;
	for (i = 0; i < NBIOSV; i++) {
		int e, tgt;

		e = FAKEBIOS + 3 * i;
		if ((gmem[e] & 0xff) != 0xc3)
			bad++;
		tgt = ((gmem[e + 2] & 0xff) << 8) | (gmem[e + 1] & 0xff);
		if ((gmem[tgt] & 0xff) != 0xed
		 || (gmem[tgt + 1] & 0xff) != 0xfe
		 || (gmem[tgt + 2] & 0xff) != HOOK_BIOS + i
		 || (gmem[tgt + 3] & 0xff) != 0xc9)
			bad++;
	}
	chk("the 17 BIOS vectors and their hooks", bad, 0);

	/* the exit stub, and the word the initial SP points at */
	chk("sp two below the exit stub", (long)G.rp[P_SP],
		(long)(FAKEEXIT - 2));
	chk("the stack holds the exit stub's address",
		(long)(((gmem[FAKEEXIT - 1] & 0xff) << 8)
		     | (gmem[FAKEEXIT - 2] & 0xff)), (long)FAKEEXIT);
	chk("the exit stub is hook 31", (long)(gmem[FAKEEXIT + 2] & 0xff),
		(long)HOOK_EXIT);

	/* a plain RET at 0x100 terminates: the whole point of the stub */
	{
		struct z80in in;

		gmem[0x100] = (char)0xc9;
		G.pc = 0x100;
		chk("ret from the entry point", z80step(&G, &in), X_OK);
		chk("... lands on the exit stub", (long)G.pc,
			(long)FAKEEXIT);
		chk("... which is a hook", z80step(&G, &in), X_HOOK);
		chk("... numbered HOOK_EXIT", z80hookno, HOOK_EXIT);
	}

	/* the command tail and the two default FCBs */
	img[0] = (char)0xc3;
	z80load(&G, gmem, img, 3L);
	chk("tail length", z80tail(&G, " b:verify.out=a:verify.in"), 25);
	chk("tail stored at 0x80", (long)(gmem[0x80] & 0xff), 25L);
	chk("tail is upper-cased", (long)(gmem[0x82] & 0xff), (long)'B');
	chk("tail is NUL-terminated", (long)(gmem[0x80 + 26] & 0xff), 0L);
	chk("fcb1 drive is B", (long)(gmem[PZ_FCB1] & 0xff), 2L);
	chk("fcb2 drive is A", (long)(gmem[PZ_FCB2] & 0xff), 1L);
	bad = 0;
	for (i = 0; i < 11; i++)
		if ((gmem[PZ_FCB1 + 1 + i] & 0xff) != "VERIFY  OUT"[i])
			bad++;
	chk("fcb1 name", bad, 0);
	bad = 0;
	for (i = 0; i < 11; i++)
		if ((gmem[PZ_FCB2 + 1 + i] & 0xff) != "VERIFY  IN "[i])
			bad++;
	chk("fcb2 name", bad, 0);
	chk("fcb1 extent cleared", (long)(gmem[PZ_FCB1 + 12] & 0xff), 0L);
	chk("fcb1 cr cleared", (long)(gmem[PZ_FCB1 + 32] & 0xff), 0L);

	/* `*' expands to `?', which is what a directory match compares */
	z80load(&G, gmem, img, 3L);
	z80tail(&G, "*.*");
	bad = 0;
	for (i = 0; i < 11; i++)
		if ((gmem[PZ_FCB1 + 1 + i] & 0xff) != '?')
			bad++;
	chk("*.* becomes eleven question marks", bad, 0);

	/* an empty tail leaves two blank FCBs and a zero length */
	z80load(&G, gmem, img, 3L);
	chk("empty tail length", z80tail(&G, ""), 0);
	chk("empty tail byte", (long)(gmem[0x80] & 0xff), 0L);
	chk("empty tail fcb1 drive", (long)(gmem[PZ_FCB1] & 0xff), 0L);
	chk("empty tail fcb1 name is blank",
		(long)(gmem[PZ_FCB1 + 1] & 0xff), (long)' ');

	/* z80rsxhdr on something that is not one */
	img[0] = (char)0xc3;
	chk("rsxhdr says no to a plain .COM",
		z80rsxhdr(img, 1024L, &r), 0);
}

/* ================================================================== */
/* 0/7. the corpus sweep and the real files			      */
/* ================================================================== */

static char cbuf[0x20000];

static long cread(const char *path)
{
	FILE *f;
	long n;

	f = fopen(path, "rb");
	if (f == 0)
		return (-1L);
	n = (long)fread(cbuf, 1, sizeof cbuf, f);
	fclose(f);
	return (n);
}

/*
 * A reachability sweep, not a linear decode.  Start at the entry
 * point, follow every branch target, stop at RET/HLT/PCHL, and never
 * decode an address twice or one outside the image.  It underestimates
 * -- a jump table reached through PCHL is invisible to it, and so is
 * anything only an indirect call reaches -- and it is still much better
 * than a linear sweep, which desynchronises into the first inline
 * string it meets and then reports whatever the desync produced.
 *
 * The honest statement of what it measures: an instruction it reports
 * IS in the image and IS reachable by a direct path; an instruction it
 * does not report may still execute.  §1.2's numbers carry that caveat
 * and the dynamic counts from section 8b are what retire it.
 */
struct census {
	long n;
	long jr, djnz, exaf, exx;
	long cb, ed, ix, ixcb, bad, hook;
};

static unsigned char cseen[0x10000];
static unsigned short cwl[0x10000];
static int cnwl;

static void sweep1at(char *mem, unsigned start, unsigned lo, unsigned hi,
	struct census *c)
{
	struct z80in in;
	unsigned a, nx;
	int i;

	memset(cseen, 0, sizeof cseen);
	cnwl = 0;
	cwl[cnwl++] = (unsigned short)start;
	memset((char *)c, 0, sizeof *c);
	while (cnwl > 0) {
		a = cwl[--cnwl];
		for (;;) {
			if (a < lo || a >= hi || cseen[a])
				break;
			cseen[a] = 1;
			z80dec(mem, (z16)a, &in);
			c->n++;
			switch (in.op) {
			case Z_JR:	c->jr++; break;
			case Z_DJNZ:	c->djnz++; break;
			case Z_EXAF:	c->exaf++; break;
			case Z_EXX:	c->exx++; break;
			case Z_CB:	c->cb++; break;
			case Z_ED:	c->ed++; break;
			case Z_IX:	c->ix++; break;
			case Z_IXCB:	c->ixcb++; break;
			case Z_HOOK:	c->hook++; break;
			case Z_BAD:	c->bad++; break;
			}
			if (in.op == Z_BAD)
				break;
			nx = (unsigned short)(a + in.len);
			if (in.op == Z_JMP
			 || (in.op == Z_JR && in.x == 4)) {
				a = in.imm;
				continue;
			}
			if (in.op == Z_RET || in.op == Z_HLT
			 || in.op == Z_PCHL)
				break;
			if (in.op == Z_JCC || in.op == Z_CCC
			 || in.op == Z_CALL || in.op == Z_JR
			 || in.op == Z_DJNZ) {
				i = (int)in.imm;
				if ((unsigned)i >= lo && (unsigned)i < hi
				 && !cseen[i])
					cwl[cnwl++] = (unsigned short)i;
			}
			a = nx;
		}
	}
}

static void sweep1(char *mem, unsigned lo, unsigned hi, struct census *c)
{
	sweep1at(mem, lo, lo, hi, c);
}

/*
 * The `-r' mode: census the RSX MODULES inside a GENCOM-bound .COM,
 * which `-c' cannot see -- it walks from 0x0100 and an RSX is entered
 * from the BDOS chain, never from the .COM's own code.
 *
 * A module is a PRL image linked for base 0, so it is placed at guest 0
 * and its own absolute addresses are already right.  Its prefix is
 * DRI's (ref/cpm3/getrsx.asm): +6 a JMP to the intercept entry, +9 a
 * JMP to the next module.  Both are walked.
 */
static void sweepr(const char *path)
{
	struct census c;
	struct comrsx r;
	long n;
	int i;
	unsigned ent;

	n = cread(path);
	if (n < 0) {
		printf("%s: unreadable\n", path);
		return;
	}
	if (!z80rsxhdr(cbuf, n, &r)) {
		printf("%s: not GENCOM-bound\n", path);
		return;
	}
	for (i = 0; i < r.n; i++) {
		long off = (long)r.off[i], len = (long)r.len[i];

		if (off < 0 || off + len > n) {
			printf("%s: RSX %d runs off the file\n", path, i);
			continue;
		}
		memset(gmem, 0, sizeof gmem);
		memcpy(gmem, cbuf + off, (size_t)len);
		ent = (gmem[7] & 0xff) | ((gmem[8] & 0xff) << 8);
		sweep1at(gmem, ent, 0, (unsigned)len, &c);
		printf("%s RSX[%d] %-8s %4ld bytes  entry %04x  reach %5ld  "
			"jr %4ld djnz %3ld exaf %2ld exx %2ld | cb %3ld "
			"ed %3ld ix %3ld ixcb %3ld bad %3ld\n",
			path, i, r.name[i], len, ent, c.n, c.jr, c.djnz,
			c.exaf, c.exx, c.cb, c.ed, c.ix, c.ixcb, c.bad);
	}
}

/* The `-c' mode: any .COM file, including the eighteen not checked in. */
static void sweep(const char *path)
{
	struct census c;
	struct comrsx r;
	long n;
	int off;

	n = cread(path);
	if (n < 0) {
		printf("%s: unreadable\n", path);
		return;
	}
	off = 0;
	if (z80rsxhdr(cbuf, n, &r)) {
		printf("%s: GENCOM-bound, comlen %u, %d RSX(es)%s\n",
			path, (unsigned)r.comlen, r.n,
			r.rsxonly ? ", RSX-ONLY" : "");
		off = RSX_HDRLEN;
	}
	memset(gmem, 0, sizeof gmem);
	if (n - off > (long)(GUESTTOP - COM_ORG))
		n = off + (GUESTTOP - COM_ORG);
	memcpy(gmem + COM_ORG, cbuf + off, (size_t)(n - off));
	sweep1(gmem, COM_ORG, (unsigned)(COM_ORG + (n - off)), &c);
	printf("%-28s %5ld bytes  reach %5ld  jr %4ld djnz %3ld "
		"exaf %2ld exx %2ld | cb %3ld ed %3ld ix %3ld ixcb %3ld\n",
		path, n, c.n, c.jr, c.djnz, c.exaf, c.exx,
		c.cb, c.ed, c.ix, c.ixcb);
}

/*
 * Section 7: the four checked-in files, and what each one is here to
 * answer.
 */
static void t_corpus(const char *dir)
{
	struct census c;
	struct comrsx r;
	char path[512];
	long n;

	/* --- PIP.COM: a plain .COM, and the biggest one we hold --- */
	sprintf(path, "%s/PIP.COM", dir);
	n = cread(path);
	ntest++;
	if (n < 0) {
		fail("PIP.COM readable", -1, 0);
		return;
	}
	chk("PIP.COM is not GENCOM-bound", z80rsxhdr(cbuf, n, &r), 0);
	memset(gmem, 0, sizeof gmem);
	memcpy(gmem + COM_ORG, cbuf, (size_t)n);
	sweep1(gmem, COM_ORG, (unsigned)(COM_ORG + n), &c);
	chk("PIP.COM decodes with no Z_BAD", c.bad, 0);
	chk("PIP.COM reaches no CB group", c.cb, 0);
	chk("PIP.COM reaches no ED group", c.ed, 0);
	chk("PIP.COM reaches no IX/IY group", c.ix + c.ixcb, 0);
	ntest++;
	if (c.jr == 0)
		fail("PIP.COM uses JR (the study says it cannot)", 0, 1);
	printf("z80test: PIP.COM static: %ld reachable, %ld jr, %ld djnz, "
		"%ld ex af\n", c.n, c.jr, c.djnz, c.exaf);

	/* --- DUMP.COM: the smallest one, first light --- */
	sprintf(path, "%s/DUMP.COM", dir);
	n = cread(path);
	chk("DUMP.COM is not GENCOM-bound", z80rsxhdr(cbuf, n, &r), 0);
	memset(gmem, 0, sizeof gmem);
	memcpy(gmem + COM_ORG, cbuf, (size_t)n);
	sweep1(gmem, COM_ORG, (unsigned)(COM_ORG + n), &c);
	chk("DUMP.COM decodes with no Z_BAD", c.bad, 0);
	chk("DUMP.COM is pure 8080", c.jr + c.djnz + c.exaf + c.exx
		+ c.cb + c.ed + c.ix + c.ixcb, 0);

	/* --- SUBMIT.COM: the 0xC9 prefix, with a program behind it --- */
	sprintf(path, "%s/SUBMIT.COM", dir);
	n = cread(path);
	chk("SUBMIT.COM is GENCOM-bound", z80rsxhdr(cbuf, n, &r), 1);
	chk("SUBMIT.COM is not RSX-only", r.rsxonly, 0);
	ntest++;
	if (r.n < 1)
		fail("SUBMIT.COM declares at least one RSX", r.n, 1);
	/* comlen is the distance from the image base to the first RSX:
	 * that is the field's whole meaning, and it is checkable. */
	ntest++;
	if (r.n >= 1 && r.off[0] != (z16)(RSX_HDRLEN + r.comlen))
		fail("SUBMIT.COM comlen reaches the first RSX",
			(long)r.off[0], (long)(RSX_HDRLEN + r.comlen));
	chk("SUBMIT.COM is accepted by the loader",
		z80load(&G, gmem, cbuf, n), CL_OK);
	printf("z80test: SUBMIT.COM: comlen %u, %d RSX(es), first \"%s\" "
		"at 0x%04x len %u\n", (unsigned)r.comlen, r.n,
		r.n ? r.name[0] : "", r.n ? (unsigned)r.off[0] : 0u,
		r.n ? (unsigned)r.len[0] : 0u);

	/* --- SAVE.COM: RSX-only.  loader3.asm:253 exists to tell this
	 * case apart, and this is the file it was written for --- */
	sprintf(path, "%s/SAVE.COM", dir);
	n = cread(path);
	chk("SAVE.COM is GENCOM-bound", z80rsxhdr(cbuf, n, &r), 1);
	chk("SAVE.COM is RSX-only", r.rsxonly, 1);
	chk("SAVE.COM comlen is one record", (long)r.comlen, 0x80L);
	chk("SAVE.COM is accepted by the loader",
		z80load(&G, gmem, cbuf, n), CL_OK);
}

/* ================================================================== */
/* 8. the seam							      */
/* ================================================================== */

/* ---- the backend switch ---- */

#define SYS_REC	0			/* record the call		*/
#define SYS_CPM	1			/* the stub filesystem		*/

static int sysmode = SYS_REC;

/* 8a's recorder */
static int rfn;
static z16 rval;
static char *raddr;
static int rret;
static long rn;

/* ---- 8b's stub CP/M ---- */

#define SF_MAX	8
#define SF_CAP	16384

struct sfile {
	char	name[11];
	int	used;
	long	len;
	char	d[SF_CAP];
};

static struct sfile sdisk[SF_MAX];
static char *sdma;
static char sbase[128];		/* where function 13 leaves the native DMA */
static int smultcnt = 1;		/* BDOS function 44's count	*/
static long sfncount[113];	/* the whole of the shim's map, 0..112 */
/* The native character control block functions 111 and 112 take. */
struct sccb {
	char	*a;
	z16	n;
};

static char scon[16384];
static int sconn;
static char skeys[256];			/* what function 10 reads	*/
static int skeyp;
static int ssearch;
static char ssname[11];

/*
 * The SCB image, as far as function 49 can see it: src/bdos/scb.c's own
 * initial values, plus the page length and the mirrors the real
 * scbsync() fills in.  A whole image rather than the three constants
 * this used to answer, because the shim now reads every word of it to
 * build the guest's copy, and a stub that answered zero everywhere
 * would make that copy agree with nothing.
 */
#define SS_LEN		100
#define SS_MAX		99		/* offsets 99 and up are refused */

static unsigned char sscb[SS_LEN];
static long ssrchp;			/* @SEARCHA, native side	*/

static void sscbreset(void)
{
	memset(sscb, 0, sizeof sscb);
	sscb[0x05] = 0x31;		/* version 3.1			*/
	sscb[0x1a] = 80;		/* console width		*/
	sscb[0x1c] = 24;		/* console page length		*/
	sscb[0x37] = '$';		/* output delimiter		*/
	sscb[0x3c] = 0x80;		/* dmaad = 0080h		*/
	sscb[0x4a] = 1;			/* @MLTIO			*/
	sscb[0x57] = 0x80;		/* long error messages		*/
	memset(sscb + 0x58, 0xff, 5);	/* the stamp bytes		*/
	ssrchp = 0;
}

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

static void skqput(const char *s)
{
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

static int skqget(void)			/* fn 6 FFh, BIOS CONIN		*/
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

/*
 * Write an FCB's random record field the way OUR BDOS writes it, which
 * is not the way an 8080's does: r0 at offset 33 is the HIGH byte
 * (src/bdos/fileio.c setran/fsize, "the same big-endian bytes").  This
 * stub had it little-endian, which is CP/M-80's order and is what the
 * guest expects to read -- so the seam's byte swap (z80bdos.c ranswap)
 * cancelled against a fixture that was already in the guest's order and
 * the two ends agreed here while disagreeing on the machine.  A stub
 * that models the wrong system passes for the wrong reason: DUMP.COM ran
 * on the host for months and printed "ERROR: No Records Exist" the first
 * time it ran on the emulator.
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
 * bytes are in the OTHER order (fcb+33 low); z80bdos.c's ranswap() is
 * what makes that true by the time this stub ever sees the FCB, for the
 * same reason DUMP.COM's function 35 needed it
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
 * Not a filesystem: the smallest thing that answers the calls a copy
 * and a dump make, so that the ANSWER can be checked.  Everything it
 * does not implement returns 0xFF and is counted, and the counts are
 * printed -- a call the guest relies on and we do not answer shows up
 * as a failed copy plus a number saying which function it was.
 */
/*
 * One record, sequential, at the FCB's current position -- the thing
 * the real BDOS calls bdosrw() (src/bdos/bdosrw.c) and the thing this
 * stub used to be all of.
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
			return (1);
		n = f->len - r * 128L;
		if (n > 128)
			n = 128;
		memset(sdma, 0x1a, 128);
		memcpy(sdma, f->d + r * 128L, (size_t)n);
	} else {
		if ((r + 1) * 128L > (long)SF_CAP)
			return (2);
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
 * inside the same block that the caller never writes read back as
 * zero rather than as leftover disk content.  This stub has no block
 * layer -- the file is a flat array -- so the equivalent guarantee is
 * made at the RECORD level: writing past the current end of file zeros
 * the gap in the array first.  That is a smaller promise than the real
 * BDOS makes (it zeros to the next block boundary, not just to the
 * record being written) but it is the same promise on every case this
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
 * MULTI-SECTOR I/O, and it is here because the machine said so.
 *
 * PIP.COM ran on the emulator through the real BDOS and copied its
 * 1,024 bytes correctly -- in 13,024 instructions and 29 BDOS calls,
 * against the 15,763 and 74 the identical interpreter had measured
 * here.  Nothing was wrong with the shim.  What was wrong was THIS
 * STUB: function 44 was in the same `return 0' group as 13, 14, 28, 37
 * and 45, so PIP's `setmulti(8)' was accepted and ignored, and PIP's
 * eight-record reads and writes came back one record at a time.  A stub
 * that answers a call by ignoring it does not fail -- it makes the
 * guest do the work again, more slowly, and the two machines disagree
 * about a program that is behaving correctly on both.
 *
 * So this is src/bdos/bdosrw.c multio() to the letter, including what
 * it does on an error: the caller's DMA address is restored, and the
 * high byte of the return value is the number of records transferred
 * before the failure (except for a physical error, code 255, whose high
 * byte already carries the extended code).  It now covers all five
 * functions the real multio() shells -- 20 and 21 sequential, 33/34/40
 * random -- and for the random three it also advances and restores the
 * FCB's own random-record field exactly as incr_rr() and multio() do:
 * one step per record transferred, the whole field put back to the
 * caller's value before returning.  Sequential I/O advances the FCB's
 * CURRENT-RECORD byte instead (sbump(), inside srw1()), which is why
 * only the random arm below touches the record field itself.
 */
static int smultio(int fn, char *addr)
{
	char *sav_dma;
	char sav33, sav34, sav35;
	int done, rtn, isran;

	isran = (fn == 33 || fn == 34 || fn == 40);

	if (smultcnt <= 1)
		return (isran ? ranw1(fn, addr) : srw1(fn, addr));

	sav_dma = sdma;
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
 * partition: BLS 4096 (bsh 5, blm 31) over 20,480 512-byte blocks, so
 * dsm is 20480/8 - 1 and exm is 1 because dsm is over 255.  DRM 511 with
 * four directory blocks reserved, a fixed disk (cks 0), no system
 * tracks.  Functions 31 and 27 answer out of this, which is what our
 * BDOS does with the block the BIOS handed it.
 */
static struct gdpb sdpb = { 64, 5, 31, 1, 0, 2559, 511, 0xF000, 0, 0 };

/* The vector a login scan leaves on an empty drive: the four directory
 * blocks and nothing else, block 0 in the TOP bit of the first byte
 * (src/bdos/dskutil.c setaloc). */
static char salv[(2559 >> 3) + 1] = { (char)0xf0 };

static int stub1(int fn, z16 val, char *addr);

/* Our BDOS works on a 36-byte copy of an FCB and writes all of it back
 * when the call ends, after any transfer into the DMA buffer.  Search
 * next ignores its parameter and uses search first's FCB. */
static int stub(int fn, z16 val, char *addr)
{
	static char *srchp;
	char t[36];
	int r;

	if (!((fn >= 15 && fn <= 23) || fn == 30 || (fn >= 33 && fn <= 36)
	 || fn == 40 || fn == 99 || fn == 100 || fn == 102 || fn == 103))
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

static int stub1(int fn, z16 val, char *addr)
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
	case 6:					/* direct console i/o	*/
		if ((val & 0xff) == 0xff)
			return (skqget());
		if ((val & 0xff) == 0xfe)
			return (skbchar || skqh < skqt ? 1 : 0);
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
	case 10: {				/* read console buffer	*/
		/* A canned session, CR-separated; a spent script answers
		 * an empty line, which is how a prompt is escaped. */
		int max = addr[0] & 0xff, len = 0;

		while (skeys[skeyp] && skeys[skeyp] != '\r' && len < max)
			addr[2 + len++] = skeys[skeyp++];
		if (skeys[skeyp] == '\r')
			skeyp++;
		addr[1] = (char)len;
		for (i = 0; i < len; i++)
			sputc(addr[2 + i] & 0x7f);
		sputc('\r');
		sputc('\n');
		return (0);
	}
	case 11:				/* console status	*/
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
	case 45: case 48:
		return (0);
	case 44:				/* set multi-sector count */
		/* src/bdos/bdosmain.c:612-616, exactly: 0 and >128 are
		 * refused and the count is left alone. */
		i = val & 0xff;
		if (i == 0 || i > 128)
			return (0xff);
		smultcnt = i;
		return (0);
	case 24:				/* login vector		*/
		return (1);
	case 25:				/* current disk = A:	*/
		return (0);
	case 29:				/* read-only vector	*/
		return (0);
	case 32:				/* get/set user code	*/
		return (0);
	case 26:				/* set DMA address	*/
		sdma = addr;
		return (0);
	case 31:			/* copy out the disk parameters	*/
		memcpy(addr, &sdpb, sizeof sdpb);
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
	case 27:			/* copy out the allocation vector */
		memcpy(addr, salv, (size_t)((sdpb.dsm >> 3) + 1));
		return (0);
	case 15:				/* open			*/
		f = sfind(addr + 1);
		if (!f)
			return (0xff);
		n = (f->len + 127) / 128 - (long)(addr[12] & 0x1f) * 128L;
		if (n < 0)
			n = 0;
		if (n > 128)
			n = 128;
		addr[15] = (char)n;
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
	case 30:				/* set file attributes	*/
		return (0);
	case 23:				/* rename		*/
		f = sfind(addr + 1);
		if (!f)
			return (0xff);
		memcpy(f->name, addr + 17, 11);
		return (0);
	case 35:				/* compute file size	*/
		f = sfind(addr + 1);
		if (!f)
			return (0xff);
		r = (f->len + 127) / 128;
		sranset(addr, r);
		return (0);
	case 36:				/* set random record	*/
		r = srec(addr);
		sranset(addr, r);
		return (0);
	case 19:				/* delete		*/
		for (i = 0, n = 0; i < SF_MAX; i++)
			if (sdisk[i].used
			 && smatch(sdisk[i].name, addr + 1, 1)) {
				sdisk[i].used = 0;
				n++;
			}
		return (n ? 0 : 0xff);
	case 17:				/* search first		*/
		memcpy(ssname, addr + 1, 11);
		ssearch = 0;
		/* @SEARCHA on the real BDOS is the host address of this
		 * FCB; a marker here, so a shim that passed the native
		 * value through would be caught answering it. */
		ssrchp = 0xaa55L;
		/* fall through */
	case 18:				/* search next		*/
		for (i = ssearch; i < SF_MAX; i++)
			if (sdisk[i].used
			 && smatch(sdisk[i].name, ssname, 1)) {
				ssearch = i + 1;
				if (sdma) {
					memset(sdma, 0, 32);
					memcpy(sdma + 1, sdisk[i].name, 11);
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
	case 49:				/* get/set SCB		*/
		/* Enough of a CP/M 3 SCB for a utility to steer by: an
		 * 80-column console, page mode off, and a version byte.
		 * Everything else reads back zero, which is what an
		 * unset SCB field is.
		 *
		 * page$mode (0x2c) IS THE ONE TO WATCH, and the comment
		 * above is loose about it: the byte is inverted in CP/M 3,
		 * so the zero this falls through to means paging is ON.
		 * That is deliberate and it is what the BDOS on the machine
		 * ships too (pm$default = PM_ON, src/bdos/bdosmisc.c) --
		 * DRI's DUMP.COM reads this byte
		 * (ref/cpm3/dump.asm:251,374-380,429) and takes a different
		 * path on it, so the two runs would diverge on any other
		 * answer and verify-z80's triple would not match.  Do not
		 * "fix" it to 0FFh without changing the system too.
		 *
		 * A SET of offset 0x4a is the real BDOS's SECOND DOOR to
		 * GBL.multcnt -- src/bdos/scb.c scb_fn() stores the byte
		 * and scbpost() (:201-210) copies it out with function
		 * 44's own clamp -- so it is here for the reason the
		 * multio() stub above exists: a stub that accepted this
		 * and ignored it would hide the very defect the DMA
		 * bound was written against. */
		i = addr[0] & 0xff;
		if (i >= SS_MAX)
			return (0xffff);
		/* scbsync(): the fields the real one refreshes from BDOS
		 * state before every access.  @SEARCHA holds a native
		 * address, which is the whole reason the shim may not
		 * pass it on. */
		sscb[0x4a] = (unsigned char)smultcnt;
		sscb[0x47] = (unsigned char)(ssrchp & 0xff);
		sscb[0x48] = (unsigned char)((ssrchp >> 8) & 0xff);
		if ((addr[1] & 0xff) == 0xff || (addr[1] & 0xff) == 0xfe) {
			sscb[i] = (unsigned char)addr[2];
			if ((addr[1] & 0xff) == 0xfe)
				sscb[i + 1] = (unsigned char)addr[3];
			/* scbpost() */
			n = sscb[0x4a];
			if (n == 0)
				n = 1;
			if (n > 128)
				n = 128;
			sscb[0x4a] = (unsigned char)n;
			smultcnt = (int)n;
			return (0);
		}
		return (sscb[i] | (sscb[i + 1] << 8));
	case 105:				/* get date and time	*/
		/* The clock our BDOS keeps in the SCB's own stamp bytes,
		 * copied out four at a time with the seconds in A. */
		sscb[0x58] = 0x34;
		sscb[0x59] = 0x12;
		sscb[0x5a] = 0x09;
		sscb[0x5b] = 0x41;
		sscb[0x5c] = 0x27;
		memcpy(addr, sscb + 0x58, 4);
		return (0x27);
	default:
		return (0xff);
	}
}

/* No GSX device here: every call is refused. */
int z80gsxd(m, g)
struct z80 *m;
char *g;
{
	return (-1);
}

/*
 * z80sys -- the one function the seam does not contain.  On the target
 * this is one line around __bdos(); here it is either the recorder
 * (8a) or the stub CP/M (8b).  Its existence is what lets the whole of
 * z80bdos.c be tested with no toolchain and no emulator.
 */
int z80sys(fn, val, addr)
int fn;
z16 val;
char *addr;
{
	rn++;
	if (sysmode == SYS_REC) {
		rfn = fn;
		rval = val;
		raddr = addr;
		return (rret);
	}
	return (stub(fn, val, addr));
}

/* ---- 8a: the calling convention, one claim at a time ---- */

static void t_seam(void)
{
	struct z80in in;
	static char img[8];
	int r;

	sysmode = SYS_REC;
	img[0] = (char)0xc3;
	z80load(&G, gmem, img, 1L);
	rn = 0;
	rret = 0;
	chk("bdosinit set a DMA address", z80bdosinit(&G), 1);
	chk("... by calling function 26", rfn, 26);
	chk("... at guest 0x0080", (long)(raddr - gmem), 0x80L);
	chk("... after reading and setting the console mode", rn, 3L);

	/* A CALL 5 reaches the hook and nothing else.  This is the whole
	 * escape mechanism end to end: the guest calls 5, page zero
	 * jumps to the fake BDOS, the fake BDOS is an ED FE 00. */
	{
		unsigned char b[3];

		b[0] = 0xcd; b[1] = 0x05; b[2] = 0x00;	/* call 5	*/
		gcode(b, 3);
	}
	G.pc = 0x100;
	chk("call 5 executes", z80step(&G, &in), X_OK);
	chk("... lands on the BDOS vector", (long)G.pc, (long)PZ_BDOS);
	chk("... which jumps", z80step(&G, &in), X_OK);
	chk("... to the fake BDOS", (long)G.pc, (long)FAKEBDOS);
	chk("... which is a hook", z80step(&G, &in), X_HOOK);
	chk("... hook 0", z80hookno, HOOK_BDOS);
	chk("... with pc past it", (long)G.pc, (long)(FAKEBDOS + 3));

	/* Console output is COLLECTED, not passed on: a byte parameter
	 * still travels in E, but E goes into the batch and the native
	 * BDOS sees nothing until something flushes it.  That is the
	 * whole of the fn 111 change, asserted at the seam. */
	z80setr(&G, R_C, 2);
	G.rp[P_DE] = 0x1241;			/* D = 0x12, E = 'A'	*/
	rfn = -1;
	chk("fn 2 runs", z80bdos(&G), B_RUN);
	chk("fn 2 reached no BDOS of its own", rfn, -1);
	chk("... and cost no gate crossing", rn, 3L);
	chk("fn 2 flushes as one call", z80oflush(), 1);
	chk("... which is function 111", rfn, 111);
	chk("... with no value parameter", (long)rval, 0L);
	{
		struct sccb *c = (struct sccb *)raddr;

		chk("... a block of one character", (long)c->n, 1L);
		chk("... which is E, not DE", (long)(c->a[0] & 0xff),
			(long)'A');
	}
	chk("an empty batch flushes nothing", z80oflush(), 0);

	/* Anything else the guest asks for flushes first, so what the
	 * console shows stays in the order the guest wrote it. */
	rfn = -1;
	z80setr(&G, R_C, 2);
	G.rp[P_DE] = 0x0042;			/* E = 'B'		*/
	z80bdos(&G);
	z80setr(&G, R_C, 11);			/* console status	*/
	z80bdos(&G);
	chk("a pending batch went out before the next function", rfn, 11);
	chk("... which is two calls, 111 then 11", rn, 6L);

	/* a word parameter travels in DE */
	z80setr(&G, R_C, 37);			/* reset drive		*/
	G.rp[P_DE] = 0x1234;
	z80bdos(&G);
	chk("fn 37 passed DE whole", (long)rval, 0x1234L);

	/* an FCB travels as a copy of the guest's bytes */
	z80setr(&G, R_C, 15);			/* open			*/
	G.rp[P_DE] = 0x5c;
	gmem[0x5c] = 0x03;
	z80bdos(&G);
	ntest++;
	if (raddr == gmem + 0x5c)
		fail("fn 15 passed a copy", 1, 0);
	chk("fn 15's copy holds the FCB", raddr[0] & 0xff, 0x03);
	chk("fn 15 passed no value", (long)rval, 0L);

	/* ... and an FCB that would run off the top is refused, with
	 * the length the call will actually touch.  36 bytes at 0xFFDC
	 * fit exactly; at 0xFFDD they do not. */
	z80setr(&G, R_C, 15);
	G.rp[P_DE] = (z16)(0x10000L - 36);
	chk("an FCB ending exactly at the top is allowed",
		z80bdos(&G), B_RUN);
	G.rp[P_DE] = (z16)(0x10000L - 35);
	chk("an FCB one byte over is refused", z80bdos(&G), B_ADDR);
	ntest++;
	if (strcmp(z80berr(), "ok") == 0)
		fail("a refusal has a sentence", 0, 1);

	/* fn 23 rename takes 52 bytes, not 36: two FCBs */
	z80setr(&G, R_C, 23);
	G.rp[P_DE] = (z16)(0x10000L - 52);
	chk("a rename ending exactly at the top is allowed",
		z80bdos(&G), B_RUN);
	G.rp[P_DE] = (z16)(0x10000L - 51);
	chk("a rename one byte over is refused", z80bdos(&G), B_ADDR);

	/* function 9's string has to terminate inside the guest */
	memset(gmem + 0xfff0, 'x', 16);
	z80setr(&G, R_C, 9);
	G.rp[P_DE] = 0xfff0;
	chk("an unterminated fn 9 string is refused", z80bdos(&G), B_ADDR);
	gmem[0xfffe] = '$';
	chk("... and a terminated one is not", z80bdos(&G), B_RUN);

	/* function 10's buffer is sized by the guest's own first byte */
	gmem[0x8000] = (char)0x7f;
	z80setr(&G, R_C, 10);
	G.rp[P_DE] = 0x8000;
	chk("a console buffer inside the guest is allowed",
		z80bdos(&G), B_RUN);
	gmem[0xff00] = (char)0xff;
	G.rp[P_DE] = 0xff00;
	chk("a console buffer that would overrun is refused",
		z80bdos(&G), B_ADDR);

	/* function 26 moves the DMA address and re-issues it */
	z80setr(&G, R_C, 26);
	G.rp[P_DE] = 0x4000;
	rfn = -1;
	chk("fn 26 runs", z80bdos(&G), B_RUN);
	chk("fn 26 re-issued the native set-DMA", rfn, 26);
	chk("fn 26 at the new address", (long)(raddr - gmem), 0x4000L);
	chk("z80dma follows", (long)z80dma, 0x4000L);
	G.rp[P_DE] = (z16)(0x10000L - 127);
	chk("a DMA address 127 from the top is refused",
		z80bdos(&G), B_ADDR);
	/* THE OBJECT of that refusal is the DMA, not the return code: the
	 * native BDOS was never told the bad address, so the shim must not
	 * be left holding it either. */
	chk("... and z80dma still holds the accepted one",
		(long)z80dma, 0x4000L);

	/* function 12 never reaches the BDOS at all */
	z80setr(&G, R_C, 12);
	rfn = -1;
	chk("fn 12 runs", z80bdos(&G), B_RUN);
	chk("fn 12 did not reach the BDOS", rfn, -1);
	chk("fn 12 answers in A", (long)G.a, (long)(z80ver & 0xff));
	chk("fn 12 answers in HL", (long)G.rp[P_HL], (long)z80ver);
	chk("fn 12 answers CP/M 3, 8080 machine", (long)z80ver, 0x0031L);

	/* function 0 terminates and never reaches the BDOS */
	z80setr(&G, R_C, 0);
	rfn = -1;
	chk("fn 0 terminates", z80bdos(&G), B_EXIT);
	chk("fn 0 did not reach the BDOS", rfn, -1);

	/* the refusals.  27 and 31 are NOT among them any more: they are
	 * answered out of the guest's own memory, in section 8e. */
	z80setr(&G, R_C, 47);
	chk("fn 47 is refused by name", z80bdos(&G), B_FN);

	/* Function 59 is not a refusal: it is the end of the chain an
	 * RSX passes a program load down to, and it answers "nothing
	 * loaded" because the shim loads one image and no more. */
	z80setr(&G, R_C, 59);
	rfn = -1;
	chk("fn 59 answers rather than refusing", z80bdos(&G), B_RUN);
	chk("... with no load done", (long)G.rp[P_HL], 0L);
	chk("... and reaches no BDOS of its own", rfn, -1);

	/* the answer comes back in A, HL and B */
	rret = 0x0409;
	z80setr(&G, R_C, 20);			/* read sequential	*/
	G.rp[P_DE] = 0x5c;
	z80bdos(&G);
	chk("a word answer in HL", (long)G.rp[P_HL], 0x0409L);
	chk("... its low half in A", (long)G.a, 0x09L);
	chk("... its high half in B", z80getr(&G, R_B), 0x04);
	rret = 0;

	/* the BIOS half: the console vectors go to BDOS function 6,
	 * because src/bdos/iosys.c refuses them through function 50 */
	z80hookno = HOOK_BIOS + 4;		/* table entry 4, CONOUT */
	z80setr(&G, R_C, 'Z');
	rfn = -1;
	chk("BIOS CONOUT runs", z80bdos(&G), B_RUN);
	chk("... through BDOS function 6", rfn, 6);
	chk("... with the character from C", (long)rval, (long)'Z');
	chk("... and z80biosfn says which vector", z80biosfn, 4);

	z80hookno = HOOK_BIOS + 2;		/* CONST		*/
	rret = 1;
	z80bdos(&G);
	chk("BIOS CONST widens 1 to 0xff", (long)G.a, 0xffL);
	rret = 0;
	z80bdos(&G);
	chk("BIOS CONST widens 0 to 0x00", (long)G.a, 0x00L);

	z80hookno = HOOK_BIOS + 3;		/* CONIN		*/
	rret = 'Q';
	z80bdos(&G);
	chk("BIOS CONIN returns the key", (long)G.a, (long)'Q');
	chk("... from our blocking read", (long)rval, 0xffL);

	/* Function 6, CP/M 3's four meanings over our two. */
	z80hookno = HOOK_BDOS;
	z80setr(&G, R_C, 6);
	G.rp[P_DE] = 0xff;
	rret = 0;
	rn = 0;
	z80bdos(&G);
	chk("fn 6 FF with no key answers 0", (long)G.a, 0L);
	chk("... after a status call only", (long)rval, 0xfeL);
	chk("... one call, so it did not wait", rn, 1L);
	rret = 'K';
	rn = 0;
	z80bdos(&G);
	chk("fn 6 FF with a key answers it", (long)G.a, (long)'K');
	chk("... through our read", (long)rval, 0xffL);
	chk("... after the status call", rn, 2L);
	G.rp[P_DE] = 0xfe;
	rret = 1;
	z80bdos(&G);
	chk("fn 6 FE widens a ready status", (long)G.a, 0xffL);
	rret = 0;
	z80bdos(&G);
	chk("fn 6 FE with no key", (long)G.a, 0L);
	G.rp[P_DE] = 0xfd;
	rret = 'W';
	rn = 0;
	z80bdos(&G);
	chk("fn 6 FD waits for a key", (long)G.a, (long)'W');
	chk("... in our read", (long)rval, 0xffL);
	chk("... and nothing else", rn, 1L);
	G.rp[P_DE] = 'x';
	z80bdos(&G);
	chk("fn 6 other E is output", (long)rval, (long)'x');
	rret = 0;

	z80hookno = HOOK_BIOS + 1;		/* warm boot		*/
	chk("BIOS warm boot terminates", z80bdos(&G), B_EXIT);
	z80hookno = HOOK_BIOS + 9;		/* SELDSK		*/
	chk("BIOS SELDSK is refused by name", z80bdos(&G), B_BIOS);

	z80hookno = HOOK_EXIT;
	chk("the exit hook terminates", z80bdos(&G), B_EXIT);
	z80hookno = HOOK_BIOS + NBIOSV;		/* past the last vector	*/
	r = z80bdos(&G);
	chk("a hook we never planted is refused", r, B_HOOKNO);
}

/* ---- 8b: a stub CP/M, and DRI's own programs running on it ---- */

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
 * The seventeen bytes drive A: comes to.  tests/i86test.c holds the same
 * list for the CP/M-86 seam, and the two have to agree: one disk, one
 * block, whichever guest is asking.
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

static void t_dparms(void)
{
	static char img[8];
	int i, bad;

	img[0] = (char)0xc9;			/* RET			*/
	z80load(&G, gmem, img, 1L);
	sysmode = SYS_CPM;
	z80bdosinit(&G);
	z80hookno = HOOK_BDOS;		/* the hook a CALL 5 would leave */

	/* The two blocks share the run between the BDOS hook and the BIOS
	 * table with nothing else: the table, its stubs and the SCB copy
	 * are all above FAKEBIOS, so that is the vector's ceiling. */
	chk("the blocks are above the BDOS hook",
		(long)(FAKEDPB >= FAKEBDOS + 3), 1L);
	chk("... and do not overlap each other",
		(long)(FAKEALV >= FAKEDPB + GDPB_LEN), 1L);
	chk("... and the furniture above them starts at FAKEBIOS",
		(long)(FAKESCB >= FAKEBIOS), 1L);

	z80setr(&G, R_C, 31);
	G.rp[P_DE] = 0;
	chk("fn 31 is answered", z80bdos(&G), B_RUN);
	chk("... with an address inside the guest", (long)G.rp[P_HL],
		(long)FAKEDPB);
	chk("... above the TPA the guest was given",
		G.rp[P_HL] >= (z16)GUESTTOP, 1);
	chk("... and clear of the BIOS vectors",
		(long)FAKEDPB + GDPB_LEN <= (long)FAKEBIOS, 1);
	for (i = 0, bad = -1; i < GDPB_LEN; i++)
		if ((gmem[FAKEDPB + i] & 0xff) != edpb[i])
			bad = i;
	chk("... holding the disk parameter block of drive A:", bad, -1);
	chk("... with A the low half of it", G.a & 0xff, FAKEDPB & 0xff);
	chk("... and B the high half", z80getr(&G, R_B), (FAKEDPB >> 8) & 0xff);

	z80setr(&G, R_C, 27);
	chk("fn 27 is answered", z80bdos(&G), B_RUN);
	chk("... with an address inside the guest", (long)G.rp[P_HL],
		(long)FAKEALV);
	chk("... and the whole vector under the BIOS vectors",
		(long)FAKEALV + ((2559 >> 3) + 1) <= (long)FAKEBIOS, 1);
	chk("... four directory blocks allocated", gmem[FAKEALV] & 0xff, 0xf0);
	chk("... and nothing after them", gmem[FAKEALV + 1] & 0xff, 0);
	chk("... to the last byte of the vector",
		gmem[FAKEALV + (2559 >> 3)] & 0xff, 0);

	sysmode = SYS_REC;
}

static void sreset(void)
{
	memset(sdisk, 0, sizeof sdisk);
	memset(sfncount, 0, sizeof sfncount);
	sconn = 0;
	sconmode = 0;
	skqh = skqt = 0;
	skbchar = 0;
	sbrkctr = 0;
	skeys[0] = '\0';
	skeyp = 0;
	sdma = 0;
	memset(sbase, 0, sizeof sbase);
	ssearch = 0;
	smultcnt = 1;
	sscbreset();
}

static void sprint(const char *tag)
{
	int i;

	printf("z80test: %s console: \"", tag);
	for (i = 0; i < sconn; i++) {
		if (scon[i] == '\r')
			continue;
		if (scon[i] == '\n')
			printf("\\n");
		else if (scon[i] < 32)
			printf("^%c", scon[i] + 64);
		else
			putchar(scon[i]);
	}
	printf("\"\n");
	printf("z80test: %s BDOS functions used:", tag);
	for (i = 0; i < (int)(sizeof sfncount / sizeof sfncount[0]); i++)
		if (sfncount[i])
			printf(" %d(%ld)", i, sfncount[i]);
	printf("\n");
}

/* ================================================================== */
/* 14. the GENCOM RSX chain					      */
/* ================================================================== */

static int gword(unsigned a);

/*
 * Build one PRL module into `img' at `at': DRI's prefix, one byte of
 * code at offset 0x40, and a relocation bitmap marking exactly two
 * bytes -- the high half of the entry JMP at offset 8, and that byte at
 * 0x40.  Both are hand-computable after the move, which is the whole
 * point of building the module here instead of borrowing one.
 */
#define RM_LEN	0x120			/* two pages, and not a round one */
#define RM_MAP	(RM_LEN / 8)
#define RM_TOT	(RM_LEN + RM_MAP)

static void rmake(char *img, long at, const char *name, int warm)
{
	int i;

	for (i = 0; i < RM_TOT; i++)
		img[at + i] = 0;
	for (i = 0; i < 6; i++)
		img[at + i] = (char)0xa5;	/* the serial area	*/
	img[at + RSXP_ENTRY] = (char)0xc3;
	img[at + RSXP_ENTRY + 1] = 0x20;
	img[at + RSXP_ENTRY + 2] = 0x01;	/* JMP 0120h: base + 20h */
	img[at + RSXP_NEXT] = (char)0xc3;
	img[at + RSXP_NEXTLO] = 0x06;
	img[at + RSXP_NEXTHI] = 0x00;
	img[at + RSXP_PREV] = 0x07;
	img[at + RSXP_PREV + 1] = 0x00;
	img[at + RSXP_WARM] = (char)warm;
	for (i = 0; i < 8; i++)
		img[at + RSXP_NAME + i] = i < (int)strlen(name)
			? name[i] : ' ';
	img[at + 0x40] = 0x02;			/* a high byte to relocate */
	img[at + RM_LEN + (8 / 8)] = (char)0x80;	/* offset 8	*/
	img[at + RM_LEN + (0x40 / 8)] = (char)0x80;	/* offset 0x40	*/
}

/* Two modules behind a 256-byte header record and a 256-byte .COM. */
#define RD_A	0x200L
#define RD_B	(RD_A + RM_TOT)
#define RD_N	(RD_B + RM_TOT)

static void rimage(char *img, int warma, int warmb, long comlen)
{
	memset(img, 0, RD_N);
	img[0] = (char)0xc9;
	img[1] = (char)(comlen & 0xff);
	img[2] = (char)((comlen >> 8) & 0xff);
	img[RSX_HDRLEN] = (char)0xc3;		/* a .COM half, not a RET */

	img[RSX_DESC0] = (char)(RD_A & 0xff);
	img[RSX_DESC0 + 1] = (char)(RD_A >> 8);
	img[RSX_DESC0 + 2] = (char)(RM_LEN & 0xff);
	img[RSX_DESC0 + 3] = (char)(RM_LEN >> 8);
	memcpy(img + RSX_DESC0 + 6, "MODA    ", 8);

	img[RSX_DESC0 + RSX_DSTRIDE] = (char)(RD_B & 0xff);
	img[RSX_DESC0 + RSX_DSTRIDE + 1] = (char)(RD_B >> 8);
	img[RSX_DESC0 + RSX_DSTRIDE + 2] = (char)(RM_LEN & 0xff);
	img[RSX_DESC0 + RSX_DSTRIDE + 3] = (char)(RM_LEN >> 8);
	memcpy(img + RSX_DESC0 + RSX_DSTRIDE + 6, "MODB    ", 8);

	rmake(img, RD_A, "MODA", warma);
	rmake(img, RD_B, "MODB", warmb);
}

static void t_rsx(const char *dir)
{
	static char img[0x10000];
	char path[256];
	long n;
	int i;

	/* ---- where the chain is allowed to live. ---- */

	chk("the RSX area ends where the furniture begins",
		(long)(GUESTTOP <= FAKEDPB), 1L);
	chk("... which is also the BDOS entry's page",
		(long)(FAKEBDOS & 0xff00), (long)GUESTTOP);
	chk("... and the link field the BDOS end of the chain takes"
	    " is clear of both",
		(long)(GUESTTOP + RSXP_PREV + 1 < FAKEDPB
		    && GUESTTOP + RSXP_PREV > FAKEBDOS + 3), 1L);
	chk("a module is addressed by page, so the entry offset is the"
	    " BDOS's own",
		(long)(FAKEBDOS - GUESTTOP), (long)RSXP_ENTRY);

	/* ---- two synthetic modules: placement, relocation, order. ---- */

	rimage(img, 0xff, 0x00, 0x100L);
	chk("a two-RSX image loads", z80load(&G, gmem, img, RD_N), CL_OK);
	chk("... placing both", z80nrsx, 2);
	/* 0x120 bytes is two pages, so each module takes the two pages
	 * below the link above it: 0xE400 - 0x200, then that - 0x200. */
	chk("... the first below the BDOS", (long)z80rsxbase[0],
		(long)(GUESTTOP - 0x200));
	chk("... the second below the first", (long)z80rsxbase[1],
		(long)(GUESTTOP - 0x400));
	chk("... and the TPA ends at the lowest", (long)z80rsxtop,
		(long)(GUESTTOP - 0x400));

	/* Relocation.  MODA sits at 0xE200, so its bias is 0xE1 -- the
	 * page LESS ONE, because the module is linked at 0x0100. */
	chk("the entry JMP's high byte took the bias",
		(long)(gmem[z80rsxbase[0] + RSXP_ENTRY + 2] & 0xff),
		(long)(0x01 + ((GUESTTOP - 0x200) >> 8) - 1));
	chk("... which makes the entry base + 0x20",
		(long)gword((unsigned)(z80rsxbase[0] + RSXP_ENTRY + 1)),
		(long)(z80rsxbase[0] + 0x20));
	chk("a marked byte inside the code took it too",
		(long)(gmem[z80rsxbase[0] + 0x40] & 0xff),
		(long)(0x02 + ((GUESTTOP - 0x200) >> 8) - 1));
	chk("... and its unmarked neighbour did not",
		(long)(gmem[z80rsxbase[0] + 0x41] & 0xff), 0L);
	chk("the second module took its own, lower bias",
		(long)(gmem[z80rsxbase[1] + 0x40] & 0xff),
		(long)(0x02 + ((GUESTTOP - 0x400) >> 8) - 1));

	/* The chain: newest first, BDOS last. */
	chk("the first module chains to the BDOS",
		(long)gword((unsigned)(z80rsxbase[0] + RSXP_NEXTLO)),
		(long)FAKEBDOS);
	chk("the second chains to the first",
		(long)gword((unsigned)(z80rsxbase[1] + RSXP_NEXTLO)),
		(long)(z80rsxbase[0] + RSXP_ENTRY));
	chk("the head's PREV is page zero's own vector",
		(long)gword((unsigned)(z80rsxbase[1] + RSXP_PREV)),
		(long)RSX_HEADPREV);
	chk("the first module's PREV names the second's NEXT high byte",
		(long)gword((unsigned)(z80rsxbase[0] + RSXP_PREV)),
		(long)(z80rsxbase[1] + RSXP_NEXTHI));
	chk("and the BDOS end of the chain names the first's",
		(long)gword((unsigned)(GUESTTOP + RSXP_PREV)),
		(long)(z80rsxbase[0] + RSXP_NEXTHI));

	chk("page zero's BDOS vector is the head module's entry",
		(long)gword(PZ_BDOS + 1),
		(long)(z80rsxbase[1] + RSXP_ENTRY));
	chk("... so a guest reading `LHLD 6' sees a TPA ending there",
		(long)(gword(PZ_BDOS + 1) - 6), (long)z80rsxtop);
	chk("... and the stack came down with it", (long)G.rp[P_SP],
		(long)(z80rsxtop - 2));
	chk("... still carrying the exit stub",
		(long)gword((unsigned)(z80rsxtop - 2)), (long)FAKEEXIT);
	chk("the serial area is cleared, not carried down",
		(long)(gmem[z80rsxbase[0]] & 0xff), 0L);
	chk("the .COM half is still at 0x100",
		(long)(gmem[COM_ORG] & 0xff), 0xc3L);
	chk("... and the program counter with it", (long)G.pc,
		(long)COM_ORG);

	/* ---- warm boot removes the flagged half of the chain. ---- */

	chk("warm boot removes the module that asked for it",
		z80rsxwboot(&G), 1);
	chk("... leaving the head where it was", (long)gword(PZ_BDOS + 1),
		(long)(z80rsxbase[1] + RSXP_ENTRY));
	chk("... and chaining the head straight to the BDOS",
		(long)gword((unsigned)(z80rsxbase[1] + RSXP_NEXTLO)),
		(long)FAKEBDOS);
	chk("... with the BDOS end chained back to the head",
		(long)gword((unsigned)(GUESTTOP + RSXP_PREV)),
		(long)(z80rsxbase[1] + RSXP_NEXTHI));
	chk("a second warm boot removes nothing", z80rsxwboot(&G), 0);

	/* Both flagged: the head goes too, and the vector rises. */
	rimage(img, 0xff, 0xff, 0x100L);
	z80load(&G, gmem, img, RD_N);
	chk("warm boot can empty the chain", z80rsxwboot(&G), 2);
	chk("... putting the BDOS vector back", (long)gword(PZ_BDOS + 1),
		(long)FAKEBDOS);

	/* ---- a module that will not fit is refused by its name. ---- */

	rimage(img, 0, 0, (long)(GUESTTOP - COM_ORG));
	chk("an RSX that would land on the program is refused",
		z80load(&G, gmem, img, RD_N), CL_RSXFIT);
	chk("... naming it", strcmp(z80rsxwho, "MODA    "), 0);
	chk("... in the sentence too",
		(long)(strstr(z80lerr(CL_RSXFIT), "MODA") != 0), 1L);

	/* ---- SUBMIT.COM and SAVE.COM, the real thing. ---- */

	sprintf(path, "%s/SUBMIT.COM", dir);
	n = cread(path);
	chk("SUBMIT.COM loads", z80load(&G, gmem, cbuf, n), CL_OK);
	chk("... with one module", z80nrsx, 1);
	/* 0x440 bytes is five pages, so it lands five pages below the
	 * BDOS, and its entry is the 0x0146 the file carries less the
	 * 0x100 it was linked at. */
	chk("... five pages below the BDOS", (long)z80rsxbase[0],
		(long)(GUESTTOP - 0x500));
	chk("... whose entry the relocation moved with it",
		(long)gword((unsigned)(z80rsxbase[0] + RSXP_ENTRY + 1)),
		(long)(z80rsxbase[0] + 0x46));
	/* File offset 0x1D of the module is the high byte of an `LDA
	 * 03F3h', and 0x03F3 less the 0x100 it was linked at is 0x2F3. */
	chk("... and an LDA inside the code with it",
		(long)gword((unsigned)(z80rsxbase[0] + 0x1c)),
		(long)(z80rsxbase[0] + 0x2f3));
	chk("... it is the module GENCOM named",
		strncmp(gmem + z80rsxbase[0] + RSXP_NAME, "GET     ", 8), 0);
	chk("... the TPA now ends at it", (long)gword(PZ_BDOS + 1) - 6,
		(long)z80rsxbase[0]);
	chk("... and the .COM half is only comlen long: the first RSX"
	    " byte did not follow it into the TPA",
		(long)(gmem[COM_ORG + 0x0f00] & 0xff), 0L);
	chk("SUBMIT's RSX asks to be removed on warm boot",
		z80rsxwboot(&G), 1);
	chk("... which puts the BDOS vector back",
		(long)gword(PZ_BDOS + 1), (long)FAKEBDOS);

	sprintf(path, "%s/SAVE.COM", dir);
	n = cread(path);
	chk("SAVE.COM loads", z80load(&G, gmem, cbuf, n), CL_OK);
	chk("... and says it is RSX-only", z80rsxonly, 1);
	chk("... its .COM half being the one RET the CCP re-runs",
		(long)(gmem[COM_ORG] & 0xff), 0xc9L);
	chk("... with its RSX placed all the same", z80nrsx, 1);
	chk("... five pages down, for 0x4B7 bytes", (long)z80rsxbase[0],
		(long)(GUESTTOP - 0x500));
	chk("SAVE's RSX stays over a warm boot", z80rsxwboot(&G), 0);
	chk("... and the BDOS vector with it", (long)gword(PZ_BDOS + 1),
		(long)(z80rsxbase[0] + RSXP_ENTRY));

	/* ---- an unbound .COM is untouched by any of it. ---- */

	sprintf(path, "%s/DUMP.COM", dir);
	n = cread(path);
	chk("DUMP.COM loads", z80load(&G, gmem, cbuf, n), CL_OK);
	chk("... with no chain at all", z80nrsx, 0);
	chk("... the BDOS vector where it always was",
		(long)gword(PZ_BDOS + 1), (long)FAKEBDOS);
	chk("... and the stack where it always was", (long)G.rp[P_SP],
		(long)(FAKEEXIT - 2));
	chk("... with nothing to remove on warm boot", z80rsxwboot(&G), 0);
}

/*
 * Run the loaded guest until it terminates or refuses.  Returns the
 * seam's last verdict; *steps gets the instruction count and *why the
 * executor's.
 */
static int grun(long limit, long *steps, int *why)
{
	long k;
	int rc, brc;

	brc = B_RUN;
	rc = X_OK;
	k = 0;
	while (k < limit) {
		rc = z80run(&G, limit - k, &k);
		if (rc != X_HOOK)
			break;
		brc = z80bdos(&G);
		if (brc != B_RUN)
			break;
		k++;
	}
	/* The same call z80.c makes when its loop ends: a run that
	 * stopped anywhere but the seam still owes the console whatever
	 * function 2 had collected. */
	z80oflush();
	*steps = k;
	*why = rc;
	return (brc);
}

static const char *xname(int rc)
{
	switch (rc) {
	case X_OK:	return ("ok");
	case X_UNIMP:	return ("X_UNIMP");
	case X_BAD:	return ("X_BAD");
	case X_HOOK:	return ("X_HOOK");
	case X_HALT:	return ("X_HALT");
	}
	return ("?");
}

/*
 * First light: DUMP.COM.  It opens a file, reads it and prints a hex
 * dump -- so its output is a known answer, computed here from the same
 * bytes, and it is the cheapest program in the corpus that reaches the
 * seam with a file in its hands.
 */
static void t_dump(const char *dir)
{
	char path[512];
	struct sfile *fi;
	long n, steps;
	int brc, why, k;

	sprintf(path, "%s/DUMP.COM", dir);
	n = cread(path);
	ntest++;
	if (n < 0) {
		fail("DUMP.COM readable", -1, 0);
		return;
	}
	chk("dump loads", z80load(&G, gmem, cbuf, n), CL_OK);
	z80tail(&G, " HELLO.BIN");

	sreset();
	fi = &sdisk[0];
	smkname(fi->name, "HELLO.BIN");
	fi->used = 1;
	fi->len = 128;
	for (k = 0; k < 128; k++)
		fi->d[k] = (char)k;

	sysmode = SYS_CPM;
	z80ninsn = z80nflag = 0;
	z80bdosinit(&G);
	brc = grun(20000000L, &steps, &why);
	sysmode = SYS_REC;

	printf("z80test: DUMP ran %ld instructions, %lu BDOS calls, "
		"%lu flag materialisations (%ld %% of instructions)\n",
		steps, (unsigned long)z80nbdos, (unsigned long)z80nflag,
		steps ? (long)((z80nflag * 100L) / (z32)steps) : 0L);
	sprint("DUMP");
	if (brc != B_EXIT)
		printf("z80test: DUMP stopped: step %s, seam %s "
			"(bdos fn %d, bios %d) at pc 0x%04x\n",
			xname(why), z80berr(), z80bdosfn, z80biosfn,
			(unsigned)G.pc);
	chk("dump exited cleanly", brc, B_EXIT);
	/* DUMP prints the first byte of the file as "00" after an
	 * address of "0000"; the check is that the bytes it printed are
	 * the bytes we put in the file, which is the known answer. */
	ntest++;
	scon[sconn] = '\0';
	if (sconn < 16 || strstr(scon, "0000: 00 01 02 03") == 0) {
		fail("dump printed the file's own bytes", sconn, 1);
		printf("     (console was \"%.60s\")\n", scon);
	}
	/* The last line of a 128-byte file is offset 0070, and the ASCII
	 * column has to be there too: a dump that stopped early or lost
	 * the right-hand column would still contain the first line. */
	ntest++;
	if (strstr(scon, "0070: 70 71 72 73") == 0)
		fail("dump reached the end of the record", 0, 1);
	ntest++;
	if (strstr(scon, "0123456789") == 0)
		fail("dump printed its ASCII column", 0, 1);
}

/*
 * The gate's own command, on the host: copy a file with DRI's PIP, then
 * compare.  The input is 1,024 bytes -- a whole number of CP/M records,
 * so a correct copy is byte-identical with no ^Z padding to argue
 * about -- and holds no 0x1A, which PIP would read as end of file.
 */
static void t_pip(const char *dir)
{
	char path[512];
	struct sfile *fi, *fo;
	long n, steps, k;
	int brc, why, bad;

	sprintf(path, "%s/PIP.COM", dir);
	n = cread(path);
	ntest++;
	if (n < 0) {
		fail("PIP.COM readable", -1, 0);
		return;
	}
	chk("pip loads", z80load(&G, gmem, cbuf, n), CL_OK);
	z80tail(&G, " VERIFY.OUT=VERIFY.IN");

	sreset();
	fi = &sdisk[0];
	smkname(fi->name, "VERIFY.IN");
	fi->used = 1;
	fi->len = 1024;
	for (k = 0; k < fi->len; k++)
		fi->d[k] = (char)(0x20 + ((k * 7 + (k >> 5)) % 0x5e));

	sysmode = SYS_CPM;
	z80ninsn = z80nflag = 0;
	z80bdosinit(&G);
	brc = grun(50000000L, &steps, &why);
	sysmode = SYS_REC;

	/*
	 * K2's number, measured rather than assumed.  The study's §4.1
	 * costs the parity fix-up at 27 target cycles on every
	 * arithmetic instruction if it is paid eagerly; this ratio is
	 * what says whether it has to be.  A rate near 100 % would mean
	 * the lazy record is being kept for nothing.
	 */
	printf("z80test: PIP ran %ld instructions, %lu BDOS calls, "
		"%lu flag materialisations (%ld %% of instructions)\n",
		steps, (unsigned long)z80nbdos, (unsigned long)z80nflag,
		steps ? (long)((z80nflag * 100L) / (z32)steps) : 0L);
	sprint("PIP");
	if (brc != B_EXIT)
		printf("z80test: PIP stopped: step %s, seam %s "
			"(bdos fn %d, bios %d) at pc 0x%04x\n",
			xname(why), z80berr(), z80bdosfn, z80biosfn,
			(unsigned)G.pc);

	chk("pip exited cleanly", brc, B_EXIT);
	/*
	 * The multi-sector path, asserted rather than assumed.  These
	 * three numbers ARE the target run: verify-z80pip greps the
	 * emulator's transcript for the instruction and BDOS-call
	 * counts this same run prints, and they agree only because the
	 * stub honours function 44 the way src/bdos/bdosrw.c multio()
	 * does.  If a later change puts function 44 back in the ignore
	 * group these fail here, on the host, in two seconds, instead
	 * of failing on the emulator as an unexplained divergence.
	 */
	chk("pip set a multi-sector count", sfncount[44], 2L);
	chk("... so 1024 bytes read in one call", sfncount[20], 1L);
	chk("... and were written in one call", sfncount[21], 1L);
	smkname(path, "VERIFY.OUT");
	fo = sfind(path);
	ntest++;
	if (fo == 0)
		fail("pip made VERIFY.OUT", 0, 1);
	else {
		chk("pip copy length", fo->len, fi->len);
		bad = 0;
		for (k = 0; k < fi->len && k < fo->len; k++)
			if (fo->d[k] != fi->d[k])
				bad++;
		chk("pip copy identical", bad, 0);
	}
}

/*
 * The RSX chain, ENTERED: DRI's SAVE.COM.
 *
 * SAVE is an RSX-only .COM -- a RET where the program would be -- and
 * the module is reached the way a CCP reaches it, by asking for the
 * command a second time with function 59.  The module takes that call,
 * points the warm-boot vector at itself and passes the call down; the
 * warm boot that follows is what enters it.  Nothing here drives the
 * module directly: the load, the chain, the relocation and the vector
 * are all doing the work, and the banner below is printed from inside
 * the relocated code.
 *
 * The known answer is the guest's own first 256 bytes, which SAVE is
 * asked to write out: page zero as the loader left it, tail and all,
 * with the BDOS vector naming the module rather than the seam.
 */
static void t_save(const char *dir)
{
	char path[512];
	struct sfile *fo;
	long n, steps, k;
	int brc, why, bad;

	sprintf(path, "%s/SAVE.COM", dir);
	n = cread(path);
	ntest++;
	if (n < 0) {
		fail("SAVE.COM readable", -1, 0);
		return;
	}
	chk("save loads", z80load(&G, gmem, cbuf, n), CL_OK);
	chk("... starting at the CCP stub, not at the RET",
		(long)G.pc, (long)FAKECCP);
	z80tail(&G, " SAVED.BIN");

	sreset();
	strcpy(skeys, "SAVED.BIN\r0000\r00FF\r");

	sysmode = SYS_CPM;
	z80ninsn = z80nflag = 0;
	z80bdosinit(&G);
	brc = grun(20000000L, &steps, &why);
	sysmode = SYS_REC;

	printf("z80test: SAVE ran %ld instructions, %lu BDOS calls, "
		"%lu flag materialisations (%ld %% of instructions)\n",
		steps, (unsigned long)z80nbdos, (unsigned long)z80nflag,
		steps ? (long)((z80nflag * 100L) / (z32)steps) : 0L);
	sprint("SAVE");
	if (brc != B_EXIT)
		printf("z80test: SAVE stopped: step %s, seam %s "
			"(bdos fn %d, bios %d) at pc 0x%04x\n",
			xname(why), z80berr(), z80bdosfn, z80biosfn,
			(unsigned)G.pc);
	chk("save exited cleanly", brc, B_EXIT);

	/* The banner lives inside the module and nowhere else, so it is
	 * on the console only if the relocated code ran. */
	ntest++;
	scon[sconn] = '\0';
	if (strstr(scon, "CP/M 3 SAVE - Version 3.1") == 0) {
		fail("the RSX was entered", 0, 1);
		printf("     (console was \"%.80s\")\n", scon);
	}
	ntest++;
	if (strstr(scon, "Beginning hex address") == 0)
		fail("the RSX asked for a range", 0, 1);

	smkname(path, "SAVED.BIN");
	fo = sfind(path);
	ntest++;
	if (fo == 0) {
		fail("save wrote the file", 0, 1);
		return;
	}
	chk("save wrote two records", fo->len, 256L);
	chk("... beginning at the warm-boot vector",
		(long)(fo->d[PZ_WBOOT] & 0xff), 0xc3L);
	/* The chain as it stood while the module ran.  SAVE takes itself
	 * out before it terminates, so the live vector is back at the
	 * seam and only the file still shows the module.
	 *
	 * Page zero above 0x005C is not checked on either machine: the
	 * range being saved is also the DMA buffer, and a BDOS is free
	 * to leave a directory record in it while the file is made. */
	chk("... whose BDOS vector names the module",
		(long)((fo->d[PZ_BDOS + 1] & 0xff)
		     | ((fo->d[PZ_BDOS + 2] & 0xff) << 8)),
		(long)(z80rsxbase[0] + RSXP_ENTRY));
	bad = 0;
	for (k = 8; k < (long)PZ_FCB1; k++)
		if (fo->d[k] != 0)
			bad++;
	chk("... and the rest of the vectors clear", bad, 0);
}

/*
 * RANDOM RECORD, fns 33/34/40, and their multi-sector shell.
 *
 * Neither DUMP nor PIP calls any of the three -- Z80-STAGE-ONE.md §0.2
 * says so and names t_pip()'s census as the thing that would notice if
 * that ever stopped being true.  It cannot notice a function nothing
 * calls, so this is that call, made directly through z80bdos() the way
 * t_seam() drives the seam -- no guest program needed, because a random
 * read or write is fully described by an FCB and a DMA address, both of
 * which this test can place in gmem[] itself.
 *
 * z80bdos.c's ranswap() sits between every call here and the stub: the
 * FCB is built and read back in the GUEST's little-endian order (r0 at
 * +33 is the low byte), and it is the seam, not this test, that flips
 * it to the order srrec()/srincr() use.  A byte-order mistake on either
 * side would show up here as the wrong record read back, which is
 * exactly the class of bug §0.2 found from DUMP's "No Records Exist".
 */
static void t_random(void)
{
	struct sfile *f;
	long k;

	sreset();
	sysmode = SYS_CPM;

	f = &sdisk[0];
	smkname(f->name, "RANDOM.DAT");
	f->used = 1;
	f->len = 4L * 128L;
	for (k = 0; k < f->len; k++)
		f->d[k] = (char)(k / 128);	/* record N is N in every byte */

	/* byte 0 of a CP/M FCB is the drive, the name starts at byte 1 --
	 * sfind(addr + 1) below is the stub's own reminder of that. */
	memset(gmem + 0x0100, 0, 36);
	smkname(gmem + 0x0100 + 1, "RANDOM.DAT");

	z80hookno = HOOK_BDOS;			/* as if a CALL 5 just landed */

	z80setr(&G, R_C, 26);			/* set DMA to 0x2000	*/
	G.rp[P_DE] = 0x2000;
	chk("fn 26 sets the DMA the random tests use", z80bdos(&G), B_RUN);

	/* ---- read random, three records in one multi-sector call ---- */

	z80setr(&G, R_C, 44);			/* multi-sector count = 3 */
	G.rp[P_DE] = 3;
	chk("fn 44 accepts a multi-sector count", z80bdos(&G), B_RUN);

	gmem[0x0100 + 33] = 1;			/* record 1, guest order:  */
	gmem[0x0100 + 34] = 0;			/* r0 (low) = 1, r1 = r2 = 0 */
	gmem[0x0100 + 35] = 0;
	z80setr(&G, R_C, 33);
	G.rp[P_DE] = 0x0100;
	chk("fn 33 multi-sector random read runs", z80bdos(&G), B_RUN);
	chk("... in one guest-visible BDOS call", sfncount[33], 1L);
	chk("... record 1 first", (long)(gmem[0x2000] & 0xff), 1L);
	chk("... record 2 next", (long)(gmem[0x2000 + 128] & 0xff), 2L);
	chk("... record 3 last", (long)(gmem[0x2000 + 256] & 0xff), 3L);
	/* multio() restores the caller's random-record field exactly;
	 * these three bytes are still in the GUEST's order because
	 * z80bdos.c un-swaps them again before returning. */
	chk("fn 33 restores the guest's r0", (long)(gmem[0x0100 + 33] & 0xff), 1L);
	chk("fn 33 restores the guest's r1", (long)(gmem[0x0100 + 34] & 0xff), 0L);
	chk("fn 33 restores the guest's r2", (long)(gmem[0x0100 + 35] & 0xff), 0L);

	/* ---- write random, single record, well within the file ---- */

	z80setr(&G, R_C, 44);			/* back to one record/call */
	G.rp[P_DE] = 1;
	z80bdos(&G);

	memset(gmem + 0x2000, (char)0xbb, 128);
	gmem[0x0100 + 33] = 2;			/* record 2		*/
	gmem[0x0100 + 34] = 0;
	gmem[0x0100 + 35] = 0;
	z80setr(&G, R_C, 34);
	G.rp[P_DE] = 0x0100;
	chk("fn 34 random write runs", z80bdos(&G), B_RUN);
	chk("... counted", sfncount[34], 1L);
	chk("... record 2 now holds the new pattern",
		(long)(f->d[2 * 128] & 0xff), 0xbbL);
	chk("... record 1 is untouched",
		(long)(f->d[1 * 128] & 0xff), 1L);

	/* ---- write random with zero fill, two records past EOF ---- */

	z80setr(&G, R_C, 44);			/* multi-sector count = 2 */
	G.rp[P_DE] = 2;
	z80bdos(&G);

	memset(gmem + 0x2000, (char)0xcc, 128);
	memset(gmem + 0x2000 + 128, (char)0xdd, 128);
	gmem[0x0100 + 33] = 10;		/* record 10, six past the	*/
	gmem[0x0100 + 34] = 0;		/* 4-record file's old EOF	*/
	gmem[0x0100 + 35] = 0;
	z80setr(&G, R_C, 40);
	G.rp[P_DE] = 0x0100;
	chk("fn 40 write-random-with-zero-fill runs", z80bdos(&G), B_RUN);
	chk("... in one guest-visible BDOS call", sfncount[40], 1L);
	chk("... record 10 holds the first write", (long)(f->d[10 * 128] & 0xff), 0xccL);
	chk("... record 11 holds the second write", (long)(f->d[11 * 128] & 0xff), 0xddL);
	/* the gap between the old 4-record EOF and record 10 reads back
	 * zero, not leftover disk content -- the promise fn 40 makes */
	chk("... the gap (record 5) is zero-filled",
		(long)(f->d[5 * 128] & 0xff), 0L);
	chk("... the gap (record 9) is zero-filled",
		(long)(f->d[9 * 128] & 0xff), 0L);

	/* ---- a random read past end of file is still an error ---- */

	z80setr(&G, R_C, 44);
	G.rp[P_DE] = 1;
	z80bdos(&G);
	gmem[0x0100 + 33] = 99;
	gmem[0x0100 + 34] = 0;
	gmem[0x0100 + 35] = 0;
	z80setr(&G, R_C, 33);
	G.rp[P_DE] = 0x0100;
	z80bdos(&G);
	chk("fn 33 past EOF answers error 1", (long)G.a, 1L);

	/* ---- a record number past the largest file CP/M can name is a
	   DIFFERENT answer: code 6, refused before the file is looked at
	   (src/bdos/bdosrw.c new_ext, `mod >= 64'). */

	gmem[0x0100 + 33] = 0;			/* record 0x040000,	*/
	gmem[0x0100 + 34] = 0;			/* in the guest's order	*/
	gmem[0x0100 + 35] = 4;
	z80setr(&G, R_C, 33);
	G.rp[P_DE] = 0x0100;
	z80bdos(&G);
	chk("fn 33 past the maximum file size answers error 6",
		(long)G.a, 6L);
	z80setr(&G, R_C, 34);
	G.rp[P_DE] = 0x0100;
	z80bdos(&G);
	chk("fn 34 past the maximum file size answers error 6",
		(long)G.a, 6L);
}

static int fcall(int fn, int de)
{
	z80hookno = HOOK_BDOS;
	z80setr(&G, R_C, fn);
	G.rp[P_DE] = (z16)de;
	z80bdos(&G);
	return (G.a & 0xff);
}

/* A sequential FCB is 33 bytes; the DMA buffer here starts right after
 * it, where a 36-byte write-back would land. */
static void t_fcbtail(void)
{
	struct sfile *f;
	int k;

	sreset();
	sysmode = SYS_CPM;
	f = &sdisk[0];
	smkname(f->name, "TAIL.DAT");
	f->used = 1;
	f->len = 128;
	for (k = 0; k < 128; k++)
		f->d[k] = (char)(0x40 + k);
	smkname(sdisk[1].name, "TAIL2.DAT");
	sdisk[1].used = 1;

	memset(gmem + 0x300, 0, 0x100);
	smkname(gmem + 0x301, "TAIL.DAT");
	memset(gmem + 0x321, 0x5a, 3);
	fcall(26, 0x321);
	chk("tail: open", fcall(15, 0x300), 0);
	chk("... leaves the bytes after the FCB", gmem[0x323] & 0xff, 0x5a);
	chk("tail: read", fcall(20, 0x300), 0);
	chk("... record lands right after the FCB", gmem[0x321] & 0xff, 0x40);
	chk("... all of it", gmem[0x323] & 0xff, 0x42);
	chk("... current record advances", gmem[0x320] & 0xff, 1);
	chk("tail: close", fcall(16, 0x300), 0);
	chk("... leaves the record", gmem[0x322] & 0xff, 0x41);

	memset(gmem + 0x300, 0, 33);
	smkname(gmem + 0x301, "OUT.DAT");
	memset(gmem + 0x321, 0x77, 128);
	chk("tail: make", fcall(22, 0x300), 0);
	chk("tail: write", fcall(21, 0x300), 0);
	chk("tail: close", fcall(16, 0x300), 0);
	chk("... the record was written", sfind(gmem + 0x301)->d[2], 0x77);
	chk("... and is still there", gmem[0x323] & 0xff, 0x77);

	memset(gmem + 0x300, 0, 0x100);
	smkname(gmem + 0x301, "TAIL????.DAT");
	chk("tail: search first", fcall(17, 0x300), 0);
	chk("... entry right after the FCB", gmem[0x322], 'T');
	chk("... TAIL.DAT", gmem[0x326], ' ');
	chk("tail: search next", fcall(18, 0), 0);
	chk("... TAIL2.DAT", gmem[0x326], '2');
	chk("tail: search ends", fcall(18, 0), 0xff);

	/* The random functions own bytes 33-35. */
	memset(gmem + 0x300, 0, 36);
	smkname(gmem + 0x301, "TAIL.DAT");
	chk("tail: compute size", fcall(35, 0x300), 0);
	chk("... r0", gmem[0x321] & 0xff, 1);
	chk("... r2", gmem[0x323] & 0xff, 0);
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
 * record lands, not what any call returns.
 */
static void t_dma13(void)
{
	struct sfile *f;

	sreset();
	sysmode = SYS_CPM;

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

	memset(gmem, 0, 0x10000);
	memset(&G, 0, sizeof G);
	G.m = gmem;
	G.f = F_ONE;
	G.lz = LZ_NONE;
	z80hookno = HOOK_BDOS;
	chk("z80bdosinit takes the default DMA", z80bdosinit(&G), 1L);

	memset(gmem + 0x0100, 0, 36);
	smkname(gmem + 0x0100 + 1, "ONE.DAT");
	memset(gmem + 0x0140, 0, 36);
	smkname(gmem + 0x0140 + 1, "TWO.DAT");

	z80setr(&G, R_C, 26);			/* a DMA of the guest's own */
	G.rp[P_DE] = 0x2000;
	chk("fn 26 sets a DMA away from the default", z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 15);
	G.rp[P_DE] = 0x0100;
	chk("fn 15 opens the first file", z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 20);
	G.rp[P_DE] = 0x0100;
	chk("fn 20 reads it", z80bdos(&G), B_RUN);
	chk("... into the guest's DMA", (long)(gmem[0x2000] & 0xff), (long)'A');

	z80setr(&G, R_C, 13);			/* reset disk system	*/
	chk("fn 13 runs", z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 14);			/* select drive A	*/
	G.rp[P_DE] = 0;
	chk("fn 14 runs", z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 15);
	G.rp[P_DE] = 0x0140;
	chk("fn 15 opens the second file", z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 20);
	G.rp[P_DE] = 0x0140;
	chk("fn 20 reads it", z80bdos(&G), B_RUN);
	chk("a read after fn 13 lands at the guest's 0x0080",
		(long)(gmem[0x0080] & 0xff), (long)'B');
	chk("... and nowhere else", (long)(sbase[0] & 0xff), 0L);
	chk("... leaving the old DMA alone",
		(long)(gmem[0x2000] & 0xff), (long)'A');
}

/* ==================================================================
 * THE DMA WINDOW A MULTI-SECTOR TRANSFER ACTUALLY USES, and the
 * length z80rsxhdr() is given.
 *
 * z80bdos.c's setdma() validated Z80DMA -- 128 bytes, ONE record --
 * while BDOS function 44 was an ordinary P_BYTE that handed the guest's
 * record count straight to the native BDOS, whose multio()
 * (src/bdos/bdosrw.c:342) then loops that many times adding SECLEN to
 * the DMA address between records.  So a guest that said "two records"
 * and put its DMA at 0xff80 -- an address setdma() accepts, because one
 * record ends exactly at the top of the 64 KB guest region -- had 256
 * bytes written from 0xff80 and the second record landed outside the
 * region altogether.  Function 49 is a second door to the same count
 * (src/bdos/scb.c:205-214 writes GBL.multcnt from the SCB mirror), so
 * the bound cannot be a one-time test at function 44; it has to be the
 * DMA check itself that knows how many records are coming.
 *
 * THE RETURN CODE IS NOT THE OBJECT.  A refused call and an accepted
 * one come back through the same three registers, and what matters is
 * whether the bytes above the guest's memory were written.  So the
 * guest memory for this section is the front of a larger array whose
 * tail holds a canary, and the check is on the canary -- "nothing
 * outside the region", which no return value can say.  Under
 * -fsanitize=address (tests/verify.mk verify-shim) the identical write
 * against gmem[] aborts the run as well; this check does not need that
 * build to see it.
 *
 * The bound is checked where the transfer happens rather than at
 * function 26, because a guest is entitled to move its DMA high while
 * the count is large and then lower the count before any I/O: it is the
 * five functions multio() shells, and only those, that must fit.
 */

#define CAN	0x5a			/* the canary byte		*/
#define CANN	128			/* how much of it there is	*/

static char gcan[0x10000 + CANN];

static int canary(void)			/* first byte written past 64 KB */
{
	int i;

	for (i = 0; i < CANN; i++)
		if ((gcan[0x10000 + i] & 0xff) != CAN)
			return (gcan[0x10000 + i] & 0xff);
	return (-1);
}

static void t_dmabound(void)
{
	struct sfile	*f;
	struct comrsx	r;
	char		*img;
	long		k;

	sreset();
	sysmode = SYS_CPM;

	f = &sdisk[0];
	smkname(f->name, "MULTI.DAT");
	f->used = 1;
	f->len = 8L * 128L;
	for (k = 0; k < f->len; k++)
		f->d[k] = (char)(0x40 + (int)(k / 128));  /* record N = '@'+N */

	memset(gcan, 0, 0x10000);
	memset(gcan + 0x10000, CAN, CANN);
	memset(&G, 0, sizeof G);
	G.m = gcan;
	G.f = F_ONE;
	G.lz = LZ_NONE;
	memset(gcan + 0x0100, 0, 36);
	smkname(gcan + 0x0100 + 1, "MULTI.DAT");

	z80hookno = HOOK_BDOS;			/* as if a CALL 5 landed */
	chk("z80bdosinit takes the default DMA", z80bdosinit(&G), 1L);

	z80setr(&G, R_C, 15);			/* open MULTI.DAT	*/
	G.rp[P_DE] = 0x0100;
	chk("fn 15 opens the multi-sector file", z80bdos(&G), B_RUN);

	/* ---- ONE record ending exactly at the top of the region is
	   legal, and the bound must leave it legal. */

	z80setr(&G, R_C, 26);
	G.rp[P_DE] = 0xff80;
	chk("fn 26 accepts a DMA whose one record ends at 0x10000",
		z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 20);
	G.rp[P_DE] = 0x0100;
	chk("fn 20 reads one record into the top of the region",
		z80bdos(&G), B_RUN);
	chk("... record 0 is there", (long)(gcan[0xff80] & 0xff), 0x40L);
	chk("... and its last byte is the region's last byte",
		(long)(gcan[0xffff] & 0xff), 0x40L);
	chk("... with nothing written above it", (long)canary(), -1L);

	/* ---- TWO records from that same DMA.  THE OBJECT: the second
	   record must not appear above the guest region. */

	z80setr(&G, R_C, 44);
	G.rp[P_DE] = 2;
	chk("fn 44 accepts a count of two", z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 20);
	G.rp[P_DE] = 0x0100;
	k = (long)z80bdos(&G);
	chk("a 2-record fn 20 from 0xff80 writes NOTHING above the guest "
	    "region", (long)canary(), -1L);
	chk("... and is refused as an address error", k, B_ADDR);
	chk("... without reaching the native BDOS a second time",
		sfncount[20], 1L);

	/* ---- the same for the other four functions multio() shells. */

	z80setr(&G, R_C, 21);
	G.rp[P_DE] = 0x0100;
	chk("a 2-record fn 21 from 0xff80 is refused", z80bdos(&G), B_ADDR);
	gcan[0x0100 + 33] = 0;
	gcan[0x0100 + 34] = 0;
	gcan[0x0100 + 35] = 0;
	z80setr(&G, R_C, 33);
	G.rp[P_DE] = 0x0100;
	chk("a 2-record fn 33 from 0xff80 is refused", z80bdos(&G), B_ADDR);
	z80setr(&G, R_C, 34);
	G.rp[P_DE] = 0x0100;
	chk("a 2-record fn 34 from 0xff80 is refused", z80bdos(&G), B_ADDR);
	z80setr(&G, R_C, 40);
	G.rp[P_DE] = 0x0100;
	chk("a 2-record fn 40 from 0xff80 is refused", z80bdos(&G), B_ADDR);
	chk("... and none of the four wrote above the region",
		(long)canary(), -1L);

	/* ---- a count of two with room for two still works, which is
	   the half of this that a blunter fix would have broken. */

	z80setr(&G, R_C, 26);
	G.rp[P_DE] = 0x2000;
	chk("fn 26 moves the DMA down", z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 20);
	G.rp[P_DE] = 0x0100;
	chk("a 2-record fn 20 with room runs", z80bdos(&G), B_RUN);
	chk("... record 1 first", (long)(gcan[0x2000] & 0xff), 0x41L);
	chk("... record 2 next", (long)(gcan[0x2000 + 128] & 0xff), 0x42L);
	chk("... in one guest-visible BDOS call", sfncount[20], 2L);

	/* ---- and the largest count function 44 accepts, placed so that
	   its last record ends exactly at the top: still legal. */

	z80setr(&G, R_C, 44);
	G.rp[P_DE] = 8;
	chk("fn 44 accepts a count of eight", z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 26);
	G.rp[P_DE] = (z16)(0x10000L - 8 * 128);
	chk("fn 26 accepts the DMA eight records fit in", z80bdos(&G), B_RUN);
	gcan[0x0100 + 33] = 0;			/* random record 0, guest */
	gcan[0x0100 + 34] = 0;			/* order: r0 low	*/
	gcan[0x0100 + 35] = 0;
	z80setr(&G, R_C, 33);
	G.rp[P_DE] = 0x0100;
	chk("an 8-record fn 33 ending at 0x10000 runs", z80bdos(&G), B_RUN);
	chk("... record 0 at the bottom of the window",
		(long)(gcan[0x10000L - 8 * 128] & 0xff), 0x40L);
	chk("... record 7 at the top", (long)(gcan[0xff80] & 0xff), 0x47L);
	chk("... and nothing above it", (long)canary(), -1L);

	/* ---- one more record than fits, by one record. */

	z80setr(&G, R_C, 44);
	G.rp[P_DE] = 9;
	z80bdos(&G);
	z80setr(&G, R_C, 33);
	G.rp[P_DE] = 0x0100;
	chk("a 9-record transfer into an 8-record window is refused",
		z80bdos(&G), B_ADDR);
	chk("... having written nothing above the region",
		(long)canary(), -1L);

	/* ---- function 49 is the other door to the count: SCB offset
	   0x4a, set-byte.  The shim must see that too. */

	z80setr(&G, R_C, 44);
	G.rp[P_DE] = 1;
	z80bdos(&G);
	gcan[0x0300] = 0x4a;			/* SCB_MLTIO		*/
	gcan[0x0301] = (char)0xff;		/* set a byte		*/
	gcan[0x0302] = 4;
	gcan[0x0303] = 0;
	z80setr(&G, R_C, 49);
	G.rp[P_DE] = 0x0300;
	chk("fn 49 sets the multi-sector count through the SCB",
		z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 26);
	G.rp[P_DE] = 0xff80;
	chk("fn 26 still accepts 0xff80", z80bdos(&G), B_RUN);
	z80setr(&G, R_C, 33);
	G.rp[P_DE] = 0x0100;
	chk("a 4-record transfer the SCB asked for is refused at 0xff80",
		z80bdos(&G), B_ADDR);
	chk("... and wrote nothing above the region", (long)canary(), -1L);

	/* ==============================================================
	 * z80rsxhdr() and the length it is given.
	 *
	 * z80load.c read img[RSX_HDRLEN] -- byte 256 -- to decide
	 * `rsxonly', after a guard that admits an image of exactly
	 * RSX_HDRLEN bytes.  The contract (z80.h) is that `n' is the
	 * image length, so on a 256-byte image that byte is not part of
	 * the image at all: the answer came from outside it.
	 */

	img = (char *)malloc(0x200);
	memset(img, 0, 0x200);
	img[0] = (char)0xc9;			/* the GENCOM prefix	*/
	img[1] = 0x00;
	img[2] = 0x01;				/* comlen 0x0100	*/
	img[0x100] = (char)0xc9;		/* the byte PAST a 256-byte
						   image -- the bait	*/
	chk("a 256-byte GENCOM header record parses",
		z80rsxhdr(img, 0x100L, &r), 1L);
	chk("... and rsxonly is NOT taken from byte 256",
		(long)r.rsxonly, 0L);
	chk("... comlen is still read", (long)r.comlen, 0x0100L);
	chk("a 257-byte image does see byte 256",
		z80rsxhdr(img, 0x101L, &r), 1L);
	chk("... and sets rsxonly from it", (long)r.rsxonly, 1L);
	free(img);

	/* The same read against a buffer of EXACTLY the declared length,
	   so an ASan build reports it as a heap overflow rather than
	   reading a neighbour quietly. */
	img = (char *)malloc(0x100);
	memset(img, 0, 0x100);
	img[0] = (char)0xc9;
	img[2] = 0x01;
	chk("a tight 256-byte buffer parses without reading past it",
		z80rsxhdr(img, 0x100L, &r), 1L);
	free(img);

	G.m = gmem;				/* leave the shared guest */
}

/* ==================================================================
 *
 * The system control block, the direct BIOS call, and the RSX call.
 *
 * Function 49 offsets 0x3A and 0x47 answer with ADDRESSES, so the
 * checks below are as much about where they point as about what they
 * say: the copy has to be inside the guest's 64 KB, above the TPA,
 * clear of the BIOS table, and holding what the native side reports.
 */

static int scbcall(int off, int set, int val)
{
	gmem[0x0300] = (char)off;
	gmem[0x0301] = (char)set;
	gmem[0x0302] = (char)(val & 0xff);
	gmem[0x0303] = (char)((val >> 8) & 0xff);
	z80hookno = HOOK_BDOS;
	z80setr(&G, R_C, 49);
	G.rp[P_DE] = 0x0300;
	return (z80bdos(&G));
}

static int bioscall(int func, int a, int bc)
{
	memset(gmem + 0x0310, 0, 8);
	gmem[0x0310] = (char)func;
	gmem[0x0311] = (char)a;
	gmem[0x0312] = (char)(bc & 0xff);
	gmem[0x0313] = (char)((bc >> 8) & 0xff);
	z80hookno = HOOK_BDOS;
	z80setr(&G, R_C, 50);
	G.rp[P_DE] = 0x0310;
	return (z80bdos(&G));
}

static int gword(unsigned a)
{
	return ((gmem[a] & 0xff) | ((gmem[a + 1] & 0xff) << 8));
}

static void t_scb(void)
{
	static char img[8];

	sreset();
	sysmode = SYS_CPM;
	img[0] = (char)0xc3;
	chk("scb: a stub image loads", z80load(&G, gmem, img, 1L), CL_OK);
	z80bdosinit(&G);

	/* the copy has somewhere of its own to live */
	chk("the SCB copy is above the TPA", (long)(FAKESCB >= GUESTTOP), 1L);
	chk("... clear of the BIOS table and its stubs",
		(long)(FAKESCB >= FAKEBIOS + 7 * NBIOSV), 1L);
	chk("... and ends inside the guest",
		(long)(FAKESCB + SCBIMGLEN <= 0x10000L), 1L);

	/* ---- 0x3A: the address of the image itself. */

	chk("fn 49 offset 0x3a is answered", scbcall(0x3a, 0, 0), B_RUN);
	chk("... with the copy's address", (long)G.rp[P_HL], (long)FAKESCB);
	chk("... which the guest can load from",
		(long)(G.rp[P_HL] != 0), 1L);

	/* and the copy holds what the native side reports */
	chk("the copy carries the version byte",
		(long)(gmem[FAKESCB + 0x05] & 0xff), 0x31L);
	chk("... the console width",
		(long)(gmem[FAKESCB + 0x1a] & 0xff), 80L);
	chk("... the page length",
		(long)(gmem[FAKESCB + 0x1c] & 0xff), 24L);
	chk("... the output delimiter",
		(long)(gmem[FAKESCB + 0x37] & 0xff), (long)'$');
	chk("... the multi-sector count",
		(long)(gmem[FAKESCB + 0x4a] & 0xff), 1L);
	chk("... and the long-message flag",
		(long)(gmem[FAKESCB + 0x57] & 0xff), 0x80L);
	chk("the copy names itself", (long)gword(FAKESCB + 0x3a),
		(long)FAKESCB);
	chk("... the guest's DMA address", (long)gword(FAKESCB + 0x3c),
		(long)PZ_DMA);
	chk("... and the guest's TPA ceiling",
		(long)gword(FAKESCB + 0x62), (long)FAKEBDOS);

	/* ---- the other address fields, through function 49 itself. */

	chk("fn 49 offset 0x3c is answered", scbcall(0x3c, 0, 0), B_RUN);
	chk("... with the DMA address", (long)G.rp[P_HL], (long)PZ_DMA);
	z80hookno = HOOK_BDOS;
	z80setr(&G, R_C, 26);
	G.rp[P_DE] = 0x2000;
	z80bdos(&G);
	scbcall(0x3c, 0, 0);
	chk("... which follows function 26", (long)G.rp[P_HL], 0x2000L);
	chk("... in the copy too", (long)gword(FAKESCB + 0x3c), 0x2000L);

	chk("fn 49 offset 0x62 is answered", scbcall(0x62, 0, 0), B_RUN);
	chk("... with the BDOS entry", (long)G.rp[P_HL], (long)FAKEBDOS);

	/* ---- 0x47: the search FCB, which is the guest's own. */

	scbcall(0x47, 0, 0);
	chk("@SEARCHA is zero before any search", (long)G.rp[P_HL], 0L);
	memset(gmem + 0x0100, 0, 36);
	smkname(gmem + 0x0101, "NOSUCH.DAT");
	z80hookno = HOOK_BDOS;
	z80setr(&G, R_C, 17);			/* search first		*/
	G.rp[P_DE] = 0x0100;
	z80bdos(&G);
	scbcall(0x47, 0, 0);
	chk("@SEARCHA is the FCB the guest gave", (long)G.rp[P_HL], 0x0100L);
	chk("... not the address the native side holds",
		(long)(G.rp[P_HL] != 0xaa55L), 1L);
	chk("... and the copy agrees", (long)gword(FAKESCB + 0x47), 0x0100L);
	/* ERASE saves it, erases, and puts it back. */
	chk("@SEARCHA takes a write", scbcall(0x47, 0xfe, 0x0100), B_RUN);
	scbcall(0x47, 0, 0);
	chk("... and reads back what was written",
		(long)G.rp[P_HL], 0x0100L);

	/* ---- the fields that name nothing the guest can reach. */

	chk("fn 49 offset 0x1e is still refused", scbcall(0x1e, 0, 0), B_FN);
	chk("a write to the SCB's own address is refused",
		scbcall(0x3a, 0xfe, 0x1000), B_FN);
	chk("a write to @MXTPA is refused", scbcall(0x62, 0xfe, 0x1000),
		B_FN);

	/* ---- what the guest writes in the copy reaches the BDOS. */

	gmem[FAKESCB + 0x1b] = 7;		/* the console column	*/
	scbcall(0x05, 0, 0);
	chk("a write in the copy reaches the native SCB",
		(long)sscb[0x1b], 7L);
	gmem[FAKESCB + 0x4a] = 4;		/* the multi-sector count */
	scbcall(0x05, 0, 0);
	chk("... including the multi-sector count", (long)smultcnt, 4L);
	chk("... and the copy still agrees",
		(long)(gmem[FAKESCB + 0x4a] & 0xff), 4L);
	gmem[FAKESCB + 0x4a] = 1;
	scbcall(0x05, 0, 0);

	/* ---- function 50, the direct BIOS call. */

	sconn = 0;
	chk("fn 50 CONOUT runs", bioscall(4, 0, 'Q'), B_RUN);
	scon[sconn] = '\0';
	chk("... and wrote the character from BC", (long)sconn, 1L);
	chk("... which is the one asked for", (long)(scon[0] & 0xff),
		(long)'Q');
	chk("... and z80bdosfn says 50", z80bdosfn, 50);
	chk("... with z80biosfn saying which vector", z80biosfn, 4);

	chk("fn 50 TIME runs", bioscall(26, 0, 0), B_RUN);
	chk("... answering the seconds in A", (long)G.a, 0x27L);
	chk("... and in HL", (long)G.rp[P_HL], 0x27L);
	chk("... and refreshes the copy's date",
		(long)gword(FAKESCB + 0x58), 0x1234L);
	chk("... its hour", (long)(gmem[FAKESCB + 0x5a] & 0xff), 0x09L);
	chk("... and its seconds",
		(long)(gmem[FAKESCB + 0x5c] & 0xff), 0x27L);

	chk("fn 50 DEVTBL runs", bioscall(20, 0, 0), B_RUN);
	chk("... naming the vector", z80biosfn, 20);
	/* The vector answers in HL, so the whole address has to survive
	 * the function-50 result convention and not its low byte. */
	chk("... answering the table's address in HL",
		(long)G.rp[P_HL], (long)FAKEDEV);
	chk("... with A the low half of it", (long)(G.a & 0xff),
		(long)(FAKEDEV & 0xff));
	chk("... and B the high half", z80getr(&G, R_B),
		(FAKEDEV >> 8) & 0xff);
	chk("fn 50 DEVINI is still refused by name",
		bioscall(21, 0, 0), B_BIOS);
	chk("... naming the vector", z80biosfn, 21);
	chk("fn 50 SELDSK is refused by name", bioscall(9, 0, 0), B_BIOS);
	chk("fn 50 warm boot terminates", bioscall(1, 0, 0), B_EXIT);
	z80hookno = HOOK_BDOS;
	z80setr(&G, R_C, 50);
	G.rp[P_DE] = (z16)(0x10000L - 7);
	chk("a BIOSPB running off the top is refused", z80bdos(&G), B_ADDR);

	/* ---- function 60 with no RSX chain loaded. */

	z80hookno = HOOK_BDOS;
	z80setr(&G, R_C, 60);
	G.rp[P_DE] = 0x0100;
	chk("fn 60 runs", z80bdos(&G), B_RUN);
	chk("... answering 0FFh: nothing handled it", (long)G.a, 0xffL);
	chk("... and the same in HL", (long)G.rp[P_HL], 0xffL);

	sysmode = SYS_REC;
}

/*
 * BIOS vector 20's device table, and the five SCB words that say where
 * each logical device goes.  The table is the third guest-resident
 * structure, after the disk parameter block and the SCB copy, and it is
 * held to the same two rules: it lives above the memory the guest was
 * given, and it does not overlap the furniture already there.
 */
static void t_devtbl(void)
{
	static char img[8];

	sreset();
	sysmode = SYS_CPM;
	img[0] = (char)0xc3;
	z80load(&G, gmem, img, 1L);
	z80bdosinit(&G);

	chk("the device table is above the TPA",
		(long)(FAKEDEV >= GUESTTOP), 1L);
	chk("... clear of the BIOS table and its stubs",
		(long)(FAKEDEV >= FAKEBIOS + 7 * NBIOSV), 1L);
	chk("... clear of the SCB copy",
		(long)(FAKEDEV >= FAKESCB + SCBIMGLEN), 1L);
	chk("... and ends inside the guest",
		(long)(FAKEDEV + FAKEDEVLEN <= 0x10000L), 1L);

	z80hookno = HOOK_BIOS + 20;
	chk("vector 20 runs", z80bdos(&G), B_RUN);
	chk("... answering the table's address", (long)G.rp[P_HL],
		(long)FAKEDEV);

	chk("the first entry is the console",
		memcmp(gmem + FAKEDEV, "CRT   ", 6), 0);
	chk("... which takes input and output",
		(long)(gmem[FAKEDEV + 6] & 0xff),
		(long)(DEVM_IN | DEVM_OUT));
	chk("... is not serial", (long)(gmem[FAKEDEV + 6] & DEVM_SERIAL),
		0L);
	chk("... and reports no baud rate",
		(long)(gmem[FAKEDEV + 7] & 0xff), 0L);

	chk("the second entry is the auxiliary line",
		memcmp(gmem + FAKEDEV + DEVENTLEN, "SIO   ", 6), 0);
	chk("... which takes input and output over a serial channel",
		(long)(gmem[FAKEDEV + DEVENTLEN + 6] & 0xff),
		(long)(DEVM_IN | DEVM_OUT | DEVM_SERIAL));
	chk("... whose rate no guest may set",
		(long)(gmem[FAKEDEV + DEVENTLEN + 6] & DEVM_SOFTBAUD), 0L);
	chk("... and which reports no baud rate either",
		(long)(gmem[FAKEDEV + DEVENTLEN + 7] & 0xff), 0L);

	chk("a zero name byte ends the list",
		(long)(gmem[FAKEDEV + 2 * DEVENTLEN] & 0xff), 0L);

	/* The rate is the BIOS's, so there is nothing for vector 21 to do
	 * but say so. */
	z80hookno = HOOK_BIOS + 21;
	chk("vector 21 is refused by name", z80bdos(&G), B_BIOS);
	chk("... naming the vector", z80biosfn, 21);

	/* ---- the assignment vectors. */

	chk("fn 49 @CIVEC is answered", scbcall(0x22, 0, 0), B_RUN);
	chk("... naming the console", (long)G.rp[P_HL],
		(long)DEVBIT(DEV_CRT));
	chk("fn 49 @COVEC names the console too", scbcall(0x24, 0, 0),
		B_RUN);
	chk("... and it is the console", (long)G.rp[P_HL],
		(long)DEVBIT(DEV_CRT));
	chk("fn 49 @LOVEC is answered", scbcall(0x2a, 0, 0), B_RUN);
	chk("... naming the console, where LIST goes", (long)G.rp[P_HL],
		(long)DEVBIT(DEV_CRT));
	chk("fn 49 @AIVEC is answered", scbcall(0x26, 0, 0), B_RUN);
	chk("... naming the auxiliary line", (long)G.rp[P_HL],
		(long)DEVBIT(DEV_SIO));
	chk("fn 49 @AOVEC names it too", scbcall(0x28, 0, 0), B_RUN);
	chk("... and it is the auxiliary line", (long)G.rp[P_HL],
		(long)DEVBIT(DEV_SIO));

	/* Our BIOS cannot redirect, so a write is a refusal and not an
	 * acceptance that does nothing. */
	chk("@COVEC takes no write", scbcall(0x24, 0xfe, 0x4000), B_FN);
	chk("... by name", strcmp(z80berr(),
		"a device assignment this BIOS cannot redirect"), 0);
	chk("... and the answer is unchanged", scbcall(0x24, 0, 0), B_RUN);
	chk("... still the console", (long)G.rp[P_HL],
		(long)DEVBIT(DEV_CRT));
	chk("@LOVEC takes no write either",
		scbcall(0x2a, 0xff, 0x40), B_FN);
	chk("the high byte of a vector is refused", scbcall(0x25, 0, 0),
		B_FN);

	/* The copy carries them as well, for a guest reading through the
	 * address 0x3A hands out. */
	scbcall(0x3a, 0, 0);
	chk("the SCB copy carries @CIVEC", (long)gword(FAKESCB + 0x22),
		(long)DEVBIT(DEV_CRT));
	chk("... and @AOVEC", (long)gword(FAKESCB + 0x28),
		(long)DEVBIT(DEV_SIO));
	/* A guest writing one into the copy must not reach the native
	 * SCB on the next function 49. */
	gmem[FAKESCB + 0x24] = 0x40;
	scbcall(0x05, 0, 0);
	chk("a vector written in the copy is put back",
		(long)gword(FAKESCB + 0x24), (long)DEVBIT(DEV_CRT));

	sysmode = SYS_REC;
}

/*
 * A guest that prints while somebody types, and then reads its own
 * keys.  With the poll left on, the burst eats them; the seam turns it
 * off for the length of the run and hands the console back after.
 */
static void t_conmode(void)
{
	static char img[8];
	int i, got[4];

	sreset();
	sysmode = SYS_CPM;
	img[0] = (char)0xc3;
	z80load(&G, gmem, img, 1L);

	sconmode = CM_NOSTOP << 1;	/* whatever the CCP was running in */
	z80bdosinit(&G);
	chk("the run turns stop-scroll off", sconmode, CM_NOSTOP);

	/* Four keys waiting, then forty characters of output: five polls,
	 * which is enough to lose all four. */
	skqput("abcd");
	z80hookno = HOOK_BDOS;
	for (i = 0; i < 40; i++) {
		z80setr(&G, R_C, 2);
		G.rp[P_DE] = (z16)('.' & 0xff);
		z80bdos(&G);
	}
	z80oflush();
	chk("the output went out", sconn, 40);

	for (i = 0; i < 4; i++) {
		z80hookno = HOOK_BIOS + 3;
		z80bdos(&G);
		got[i] = G.a & 0xff;
	}
	chk("BIOS CONIN got the first key", got[0], 'a');
	chk("... the second", got[1], 'b');
	chk("... the third", got[2], 'c');
	chk("... and the fourth", got[3], 'd');

	chk("the guest's exit gives the console back", z80bdosfini(), 1);
	chk("... in the mode it found", sconmode, CM_NOSTOP << 1);
	chk("... once", z80bdosfini(), 0);

	/* The same burst with the poll on, which is what the console does
	 * to a guest the seam has not spoken for. */
	sreset();
	z80bdosinit(&G);
	sconmode = 0;
	skqput("abcd");
	z80hookno = HOOK_BDOS;
	for (i = 0; i < 40; i++) {
		z80setr(&G, R_C, 2);
		G.rp[P_DE] = (z16)('.' & 0xff);
		z80bdos(&G);
	}
	z80oflush();
	z80hookno = HOOK_BIOS + 3;
	z80bdos(&G);
	chk("the poll keeps only the last key it took", (long)(G.a & 0xff),
		(long)'d');
	z80bdos(&G);
	chk("... and the ones before it are gone", (long)(G.a & 0xff), 0L);

	z80bdosfini();
	sysmode = SYS_REC;
}

/* ================================================================== */

/*
 * The `-x' mode: load one corpus .COM with a command tail and run it
 * against the stub, then say where it stopped and which BDOS functions
 * and BIOS vectors it reached.  This is the dynamic half of the gap
 * measurement: a seam refusal is B_FN/B_BIOS with z80berr() naming it,
 * and an executor refusal is X_UNIMP/X_BAD at a printed pc.
 *
 * A GENCOM-bound image is loaded WITHOUT its 256-byte header, which is
 * not what the loader does (it refuses); it is a way to ask what the
 * .COM half alone reaches before the RSX question is answered.
 */
/*
 * The same loop, but a seam refusal is RECORDED and the guest is let
 * through with the "no" a real BDOS gives for a function it does not
 * have (A = 0FFh, HL = 0FFFFh).  Without this a program's gap list is
 * truncated at its FIRST refusal and the second one is never seen.
 * What it measures is therefore a superset path, not a correct run.
 */
static long refused[113];
static long refusedbios[NBIOSV];
static long refscb[256];

static int grunt(long limit, long *steps, int *why)
{
	struct z80in in;
	long k, nref;
	int rc, brc;

	memset(refused, 0, sizeof refused);
	memset(refusedbios, 0, sizeof refusedbios);
	memset(refscb, 0, sizeof refscb);
	brc = B_RUN;
	rc = X_OK;
	nref = 0;
	for (k = 0; k < limit; k++) {
		rc = z80step(&G, &in);
		if (rc == X_OK)
			continue;
		if (rc != X_HOOK)
			break;
		brc = z80bdos(&G);
		if (brc == B_RUN)
			continue;
		if ((brc == B_FN || brc == B_BIOS) && ++nref < 2000) {
			if (brc == B_BIOS && z80biosfn >= 0
			 && z80biosfn < NBIOSV)
				refusedbios[z80biosfn]++;
			else if (z80bdosfn >= 0 && z80bdosfn <= 112) {
				refused[z80bdosfn]++;
				if (z80bdosfn == 49)
					refscb[gmem[G.rp[P_DE]] & 0xff]++;
				if (z80bdosfn == 50)
					/* the BIOSPB: func, A, BC, DE, HL */
					printf("z80test:   fn 50 BIOSPB func "
						"%d, a %02x, bc %02x%02x\n",
						gmem[G.rp[P_DE]] & 0xff,
						gmem[(z16)(G.rp[P_DE]+1)]&0xff,
						gmem[(z16)(G.rp[P_DE]+3)]&0xff,
						gmem[(z16)(G.rp[P_DE]+2)]&0xff);
			}
			G.a = 0xff;
			G.rp[P_HL] = 0xffff;
			brc = B_RUN;
			continue;
		}
		break;
	}
	z80oflush();
	*steps = k;
	*why = rc;
	return (brc);
}

static int tolerate;

static void runx(const char *path, const char *tail)
{
	struct comrsx r;
	struct sfile *fi;
	long n, steps, k;
	int brc, why, off;

	n = cread(path);
	if (n < 0) {
		printf("%s: unreadable\n", path);
		return;
	}
	off = 0;
	if (z80rsxhdr(cbuf, n, &r))
		printf("z80test: %s is GENCOM-bound (%d RSX%s%s)\n", path, r.n,
			r.n == 1 ? "" : "es", r.rsxonly ? ", RSX-ONLY" : "");
	off = z80load(&G, gmem, cbuf, n);
	if (off != CL_OK) {
		printf("z80test: %s did not load: %s\n", path, z80lerr(off));
		return;
	}
	if (z80nrsx) {
		int i;

		printf("z80test: %s chain:", path);
		for (i = 0; i < z80nrsx; i++)
			printf(" %.8s@%04x", gmem + z80rsxbase[i] + RSXP_NAME,
				(unsigned)z80rsxbase[i]);
		printf(", TPA top %04x, bdos vector %04x\n",
			(unsigned)z80rsxtop,
			(unsigned)(z80rw(&G, (z16)(PZ_BDOS + 1))));
	}
	z80tail(&G, (char *)tail);

	sreset();
	fi = &sdisk[0];
	smkname(fi->name, "VERIFY.IN");
	fi->used = 1;
	fi->len = 1024;
	for (k = 0; k < fi->len; k++)
		fi->d[k] = (char)(k & 0x7f);

	/* SUBMIT's input and its only real work: it reads a .SUB and
	 * writes $$$.SUB with the lines reversed and padded to records. */
	fi = &sdisk[1];
	smkname(fi->name, "VERIFY.SUB");
	fi->used = 1;
	memcpy(fi->d, "DIR\r\nERA X.Y\r\n", 14);
	fi->len = 14;

	sysmode = SYS_CPM;
	z80ninsn = z80nflag = 0;
	z80bdosinit(&G);
	brc = tolerate ? grunt(5000000L, &steps, &why)
		       : grun(5000000L, &steps, &why);
	sysmode = SYS_REC;

	printf("z80test: %s tail \"%s\": %ld instructions, %lu BDOS calls\n",
		path, tail, steps, (unsigned long)z80nbdos);
	printf("z80test: %s stopped: %s, seam %s (bdos fn %d, bios %d) "
		"at pc 0x%04x\n", path,
		brc == B_EXIT ? "exit" : xname(why), z80berr(),
		z80bdosfn, z80biosfn, (unsigned)G.pc);
	if (brc != B_EXIT && z80bdosfn == 49)
		printf("z80test: %s SCB offset 0x%02x, set flag 0x%02x\n",
			path, gmem[G.rp[P_DE]] & 0xff,
			gmem[(z16)(G.rp[P_DE] + 1)] & 0xff);
	if (tolerate) {
		int i;

		printf("z80test: %s REFUSED bdos fns:", path);
		for (i = 0; i <= 112; i++)
			if (refused[i])
				printf(" %d(%ld)", i, refused[i]);
		printf("\nz80test: %s REFUSED bios vectors:", path);
		for (i = 0; i < NBIOSV; i++)
			if (refusedbios[i])
				printf(" %d(%ld)", i, refusedbios[i]);
		printf("\nz80test: %s REFUSED scb offsets:", path);
		for (i = 0; i < 256; i++)
			if (refscb[i])
				printf(" 0x%02x(%ld)", i, refscb[i]);
		printf("\n");
	}
	sprint(path);
	for (k = 0; k < SF_MAX; k++) {
		long j;

		if (!sdisk[k].used || !sdisk[k].len)
			continue;
		printf("z80test: %s left %.11s, %ld bytes:", path,
			sdisk[k].name, sdisk[k].len);
		for (j = 0; j < sdisk[k].len && j < 48; j++)
			printf(" %02x", sdisk[k].d[j] & 0xff);
		printf("\n");
	}
}

int main(argc, argv)
int argc;
char **argv;
{
	int i;

	if (argc > 2 && strcmp(argv[1], "-c") == 0) {
		for (i = 2; i < argc; i++)
			sweep(argv[i]);
		return (0);
	}
	if (argc > 2 && (strcmp(argv[1], "-x") == 0
			|| strcmp(argv[1], "-X") == 0)) {
		tolerate = (argv[1][1] == 'X');
		runx(argv[2], argc > 3 ? argv[3] : "");
		return (0);
	}
	if (argc > 2 && strcmp(argv[1], "-r") == 0) {
		for (i = 2; i < argc; i++)
			sweepr(argv[i]);
		return (0);
	}
	/* The corpus directory is REQUIRED, not optional.  A default
	 * that skipped sections 7 and 8b when it was not named would
	 * turn a broken make rule into a smaller passing run. */
	if (argc != 2) {
		fprintf(stderr, "usage: z80test <corpusdir>\n");
		fprintf(stderr, "       z80test -c FILE.COM ...\n");
		return (2);
	}

	t_lengths();
	t_allbytes();
	t_flags_alu();
	t_flags_incdec();
	t_flags_daa();
	t_flags_dad();
	t_flags_rot();
	t_flags_psw();
	t_exec();
	t_cb_rot();
	t_cb_bit();
	t_cb_setres();
	t_ed_ldblk();
	t_ed_cpblk();
	t_ed_arith();
	t_lazyrate();
	t_delay();
	t_lazycond();
	t_refuse();
	t_loader();
	t_corpus(argv[1]);
	t_seam();
	t_dparms();
	t_dump(argv[1]);
	t_pip(argv[1]);
	t_random();
	t_fcbtail();
	t_dma13();
	t_dmabound();
	t_scb();
	t_devtbl();
	t_conmode();
	t_rsx(argv[1]);
	t_save(argv[1]);

	printf("z80test: %d checks, %d failures\n", ntest, nfail);
	return (nfail != 0);
}
