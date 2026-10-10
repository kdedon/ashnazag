/*
 * zdiff -- the 68k decoder and run loop for the 8080/Z80 against the C
 * references, on the target.
 *
 *	zdiff		the comparisons; exit 0 when every case agrees
 *	zdiff -t N	time N million instructions of a loop, both run loops
 *
 * The C decoder is linked as z80decc and the C executor and run loop as
 * cz80*, over two 64 KB guests holding the same bytes.  Decoder: every
 * opcode, ten operand patterns, five PCs including the wrap.  Run loop:
 * every opcode from random states (operands, SP and PC near 0xFFFF,
 * pending records of every class, delay loops), then runs of 64
 * instructions through random memory.  Every register, the lazy record,
 * the counters and the memory must match.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "z80.h"

extern int z80decc(), cz80run();
extern z32 cz80ninsn, cz80nflag, cz80nskip;
extern int cz80fast, cz80hookno;
extern int (*cz80wait)();

#define	NSTATE	96
#define	NSEQ	96
#define	SEQLEN	64L

static unsigned rs = 0xace1;

static unsigned
rnd()
{
	rs ^= rs << 7;
	rs ^= rs >> 9;
	rs ^= rs << 8;
	return rs & 0xffff;
}

/* ---- the decoders */

static char pat[10][3] = {
	{ 0x00, 0x00, 0x00 }, { 0xff, 0xff, 0xff }, { 0x80, 0x7f, 0x01 },
	{ 0xfe, 0x40, 0x63 }, { 0xfe, 0x01, 0x05 }, { 0x43, 0x34, 0x12 },
	{ 0xcb, 0x05, 0x77 }, { 0x36, 0x12, 0x34 }, { 0x7e, 0x11, 0x22 },
	{ 0xdd, 0x21, 0x00 }
};
static z16 pcs[5] = { 0x0100, 0x0000, 0x1234, 0xfffd, 0xffff };

static int
decsame(a, c, ra, rc)
	struct z80in *a, *c;
	int ra, rc;
{
	return ra == rc && a->len == c->len && a->op == c->op && a->fl == c->fl &&
	    a->x == c->x && a->y == c->y && a->pfx == c->pfx && a->sub == c->sub &&
	    a->imm == c->imm && a->disp == c->disp;
}

static int
zdec(m)
	char *m;
{
	struct z80in a, c;
	int k, p, op, ra, rc;
	unsigned n = 0;
	z16 pc;

	for (k = 0; k < 5; k++)
		for (p = 0; p < 10; p++)
			for (op = 0; op < 256; op++) {
				pc = pcs[k];
				m[pc] = op;
				m[(z16)(pc + 1)] = pat[p][0];
				m[(z16)(pc + 2)] = pat[p][1];
				m[(z16)(pc + 3)] = pat[p][2];
				memset((char *)&a, 0x55, sizeof a);
				memset((char *)&c, 0x55, sizeof c);
				ra = z80dec(m, pc, &a);
				rc = z80decc(m, pc, &c);
				if (!decsame(&a, &c, ra, rc)) {
					printf("FAIL z80dec pc %04x op %02x pat %d: asm len %d op %d imm %04x, c len %d op %d imm %04x\n",
					    pc, op, p, a.len, a.op, a.imm, c.len, c.op, c.imm);
					return 0;
				}
				n++;
			}
	printf("PASS z80dec %u decodes agree\n", n);
	return 1;
}

/* ---- the run loops */

static char *ma, *mc;
static struct z80 S, A, C;
static unsigned ncase;

static void
bad(what, av, cv)
	char *what;
	unsigned av, cv;
{
	printf("FAIL z80run %s asm=%04x c=%04x case %u from pc=%04x op=%02x\n",
	    what, av, cv, ncase, S.pc, ma[S.pc] & 0xff);
}

static void
poke(a, v)
	z16 a;
	int v;
{
	ma[a] = v;
	mc[a] = v;
}

static int
memsame(a)
	z16 a;
{
	if (ma[a] == mc[a])
		return 1;
	bad("mem", a, 0);
	printf("     asm=%02x c=%02x\n", ma[a] & 0xff, mc[a] & 0xff);
	return 0;
}

static int
allsame()
{
	z16 a = 0;

	do
		if (ma[a] != mc[a])
			return memsame(a);
	while (++a != 0);
	return 1;
}

static int
near(s)
	struct z80 *s;
{
	z16 a;
	int i;

	for (i = 0; i < 4; i++)
		if (!memsame((z16)(s->pc + i)))
			return 0;
	for (i = -3; i < 3; i++)
		if (!memsame((z16)(s->rp[P_SP] + i)))
			return 0;
	for (i = 0; i < 3; i++)
		if (!memsame(s->rp[i]) || !memsame((z16)(s->rp[i] + 1)))
			return 0;
	a = (z16)((ma[(z16)(s->pc + 1)] & 0xff) | (ma[(z16)(s->pc + 2)] & 0xff) << 8);
	return memsame(a) && memsame((z16)(a + 1));
}

static char edge[10] = { 0, 1, 2, 0x0f, 0x10, 0x7f, 0x80, 0x81, 0xfe, 0xff };

static int
sv()
{
	unsigned r = rnd();

	if (r & 0x100)
		return edge[(r >> 9) % 10] & 0xff;
	return r & 0xff;
}

static void
mkrec()
{
	int a, b, c;

	S.lz = rnd() % 11;
	S.lc = rnd() & 1;
	a = sv();
	b = sv();
	c = S.lc;
	S.la = a;
	S.lb = b;
	S.lr = sv();
	if (rnd() & 3)
		switch (S.lz) {
		case LZ_ADD: S.lr = (a + b + c) & 0xff; break;
		case LZ_SUB: S.lr = (a - b - c) & 0xff; break;
		case LZ_AND: S.lr = a & b; break;
		case LZ_LOG: S.lr = a ^ b; break;
		case LZ_INR: S.lb = 1; S.lr = (a + 1) & 0xff; break;
		case LZ_DCR: S.lb = 1; S.lr = (a - 1) & 0xff; break;
		}
}

static void
mkstate()
{
	int i;

	for (i = 0; i < 4; i++)
		S.rp[i] = sv() << 8 | sv();
	for (i = 0; i < 3; i++)
		S.arp[i] = rnd();
	S.ix = rnd();
	S.iy = rnd();
	S.a = sv();
	S.f = rnd();
	S.aa = rnd();
	S.af = rnd();
	S.iff = rnd() & 1;
	S.halt = 0;
	S.pc = rnd();
	switch (rnd() & 7) {
	case 0: S.pc = 0xfffc + (rnd() & 3); break;
	case 1: S.rp[P_SP] = rnd() & 3; break;
	case 2: S.rp[P_HL] = 0xffff; break;
	}
	mkrec();
}

static void
plant(op)
	int op;
{
	int b1 = sv(), b2 = sv(), b3 = rnd() & 0xff, i;
	z16 a;

	if ((op & 0xc7) == 0x05 && op != 0x35 && (rnd() & 1)) {
		/* a DCR r delay loop: JNZ or JR NZ back to it */
		if (rnd() & 1) {
			b1 = 0xc2;
			b2 = S.pc & 0xff;
			b3 = S.pc >> 8 & 0xff;
		} else {
			b1 = 0x20;
			b2 = 0xfd;
		}
		if (op == 0x3d)
			S.a &= 0x0f;
		else if (rnd() & 1)
			S.rp[op >> 4 & 3] &= (op & 8) ? 0xff0f : 0x0fff;
	}
	if (op == 0xed && (rnd() & 3) == 0)
		b1 = 0xfe;
	for (i = -3; i < 3; i++)
		poke((z16)(S.rp[P_SP] + i), rnd());
	for (i = 0; i < 3; i++) {
		poke(S.rp[i], sv());
		poke((z16)(S.rp[i] + 1), sv());
	}
	a = b1 | b2 << 8;
	poke(a, rnd());
	poke((z16)(a + 1), rnd());
	poke(S.pc, op);
	poke((z16)(S.pc + 1), b1);
	poke((z16)(S.pc + 2), b2);
	poke((z16)(S.pc + 3), b3);
}

static int
step(n)
	long n;
{
	long ka = 0, kc = 0;
	int ra, rc, i;

	A = S;
	A.m = ma;
	C = S;
	C.m = mc;
	z80ninsn = z80nflag = z80nskip = 0;
	cz80ninsn = cz80nflag = cz80nskip = 0;
	z80fast = cz80fast = (rnd() & 7) != 0;
	z80wait = cz80wait = 0;
	ra = z80run(&A, n, &ka);
	rc = cz80run(&C, n, &kc);
	ncase++;
	if (ra != rc)
		return bad("answer", ra, rc), 0;
	if (ka != kc)
		return bad("count", (unsigned)ka, (unsigned)kc), 0;
	if (ra == X_HOOK && z80hookno != cz80hookno)
		return bad("hook", z80hookno, cz80hookno), 0;
	if (z80ninsn != cz80ninsn)
		return bad("ninsn", (unsigned)z80ninsn, (unsigned)cz80ninsn), 0;
	if (z80nflag != cz80nflag)
		return bad("nflag", (unsigned)z80nflag, (unsigned)cz80nflag), 0;
	if (z80nskip != cz80nskip)
		return bad("nskip", (unsigned)z80nskip, (unsigned)cz80nskip), 0;
	for (i = 0; i < 4; i++)
		if (A.rp[i] != C.rp[i])
			return bad("pair", A.rp[i], C.rp[i]), 0;
	for (i = 0; i < 3; i++)
		if (A.arp[i] != C.arp[i])
			return bad("alt pair", A.arp[i], C.arp[i]), 0;
	if (A.a != C.a || A.f != C.f)
		return bad("af", A.a << 8 | A.f, C.a << 8 | C.f), 0;
	if (A.aa != C.aa || A.af != C.af)
		return bad("af'", A.aa << 8 | A.af, C.aa << 8 | C.af), 0;
	if (A.pc != C.pc)
		return bad("pc", A.pc, C.pc), 0;
	if (A.ix != C.ix || A.iy != C.iy)
		return bad("ix", A.ix, C.ix), 0;
	if (A.iff != C.iff || A.halt != C.halt)
		return bad("iff/halt", A.iff << 8 | A.halt, C.iff << 8 | C.halt), 0;
	if (A.lz != C.lz || A.lc != C.lc)
		return bad("lz/lc", A.lz << 8 | A.lc, C.lz << 8 | C.lc), 0;
	if (A.lz != LZ_NONE && (A.la != C.la || A.lb != C.lb || A.lr != C.lr))
		return bad("record", A.la, C.la), 0;
	return 1;
}

static int
zrun()
{
	int op, k;
	z16 a = 0;

	do
		poke(a, rnd());
	while (++a != 0);
	ncase = 0;
	for (op = 0; op < 256; op++) {
		for (k = 0; k < NSTATE; k++) {
			mkstate();
			plant(op);
			if (!step(1L) || !near(&S) || !near(&C))
				return 0;
		}
		if ((op & 31) == 31 && !allsame())
			return 0;
	}
	mkstate();
	if (!step(0L) || A.pc != S.pc)
		return 0;
	for (k = 0; k < NSEQ; k++) {
		mkstate();
		if (!step(SEQLEN) || !allsame())
			return 0;
	}
	printf("PASS z80run %u runs agree\n", ncase);
	return 1;
}

static int
segsame()
{
	int i;

	memset(ma, 0x5a, 0x10000);
	segclr(ma);
	for (i = 0; i < 0x10000; i++)
		if (ma[i]) {
			printf("FAIL segclr at %04x\n", i);
			return 0;
		}
	printf("PASS segclr\n");
	return 1;
}

static long
ms()
{
	struct timeval tv;

	gettimeofday(&tv, (struct timezone *)0);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* the BENCH80 loop: 40 x 65536 x (DCX B, MOV A,B, ORA C, JNZ), then HLT */
static void
bench(mil)
	long mil;
{
	static unsigned char prg[] = {
		0x16, 40, 0x01, 0, 0, 0x0b, 0x78, 0xb1, 0xc2, 0x05, 0x01,
		0x15, 0xc2, 0x02, 0x01, 0x76
	};
	long t, k, n = mil * 1000000L;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		memset(ma, 0, 0x10000);
		memcpy(ma + 0x100, (char *)prg, sizeof prg);
		memset((char *)&A, 0, sizeof A);
		A.m = ma;
		A.pc = 0x100;
		A.f = F_ONE;
		k = 0;
		t = ms();
		while (k < n)
			if ((pass ? cz80run(&A, n - k, &k) : z80run(&A, n - k, &k)) != X_OK) {
				A.pc = 0x100;
				k++;
			}
		t = ms() - t;
		printf("INFO z80 %s run loop: %ld instructions in %ld ms\n", pass ? "C" : "68k", k, t);
	}
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	if ((ma = malloc(0x10000)) == 0 || (mc = malloc(0x10000)) == 0)
		return 1;
	if (argc == 3 && strcmp(argv[1], "-t") == 0) {
		bench(atol(argv[2]));
		return 0;
	}
	return segsame() && zdec(ma) && zrun() ? 0 : 1;
}
