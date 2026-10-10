/*
 * idiff -- the 68k decoder and run loop for the 8086 against the C
 * references, on the target.
 *
 *	idiff		the comparisons; exit 0 when every case agrees
 *	idiff -t N	time N million instructions of a loop, both run loops
 *
 * Decoder: every opcode under sixteen prefix runs and five IPs at the
 * wrap, with every second byte.  Run loop: i86runa() against i86run()
 * from random states over two images holding the same bytes, mostly the
 * classes the assembly runs, in every mod r/m form, with segment
 * overrides, biased segments, the trap flag and delay loops.  Every
 * field of the machine, the counters and the memory must match.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "i86.h"

extern int i86decc();

#define	IMG	0x20000L	/* a segment base may sit anywhere in the first 64 KB */

/* ---- the decoders */

static char pfx[][17] = {
	{ 0 },
	{ 1, 0x26 }, { 1, 0x2e }, { 1, 0x36 }, { 1, 0x3e },
	{ 1, 0xf0 }, { 1, 0xf1 }, { 1, 0xf2 }, { 1, 0xf3 },
	{ 2, 0xf3, 0x26 },
	{ 3, 0x36, 0xf2, 0x2e },
	{ 4, 0xf0, 0x3e, 0xf3, 0x26 },
	{ 13, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26,
	  0x26, 0x26, 0x36 },
	{ 14, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e,
	  0x3e, 0x3e, 0x3e, 0x2e },
	{ 15, 0xf3, 0xf3, 0xf3, 0xf3, 0xf3, 0xf3, 0xf3, 0xf3, 0xf3, 0xf3,
	  0xf3, 0xf3, 0xf3, 0xf3, 0x26 },
	{ 16, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2,
	  0xf2, 0xf2, 0xf2, 0xf2, 0x36, 0xf0 }
};

static struct {
	int p;
	i16 ip;
} pass[] = {
	{ 0, 0x0100 },
	{ 1, 0x0002 }, { 2, 0x0100 }, { 3, 0x0002 }, { 4, 0x0100 },
	{ 5, 0x0002 }, { 6, 0x0100 }, { 7, 0x0002 }, { 8, 0x0100 },
	{ 9, 0x0002 }, { 10, 0x0100 }, { 11, 0x0002 },
	{ 12, 0x0002 }, { 13, 0x0100 }, { 14, 0x0002 }, { 15, 0x0100 },
	{ 0, 0xffff }, { 0, 0xfffe }, { 0, 0xfffd }, { 0, 0xfffc },
	{ 0, 0xfffb }
};
#define	NPASS	(sizeof pass / sizeof pass[0])

static int
decsame(a, c, ra, rc)
	struct i86in *a, *c;
	int ra, rc;
{
	return ra == rc && a->len == c->len && a->op == c->op && a->fl == c->fl &&
	    a->w == c->w && a->x == c->x && a->mod == c->mod && a->reg == c->reg &&
	    a->rm == c->rm && a->seg == c->seg && a->disp == c->disp &&
	    a->imm == c->imm && a->imm2 == c->imm2;
}

static int
idec(m)
	char *m;
{
	struct i86in a, c;
	int k, j, n, p, op, g, gmax, ra, rc;
	unsigned long nd = 0;
	i16 ip, q;

	for (k = 0; k < NPASS; k++) {
		p = pass[k].p;
		n = pfx[p][0];
		ip = pass[k].ip - n;
		for (j = 0; j < n; j++)
			m[(i16)(ip + j)] = pfx[p][1 + j];
		q = ip + n;
		gmax = n > 14 ? 1 : 256;
		for (op = 0; op < 256; op++) {
			m[q] = op;
			for (g = 0; g < gmax; g++) {
				m[(i16)(q + 1)] = g;
				m[(i16)(q + 2)] = g ^ 0x80;
				m[(i16)(q + 3)] = ~g;
				m[(i16)(q + 4)] = g + 0x7f;
				m[(i16)(q + 5)] = g ^ 0x5a;
				memset((char *)&a, 0x55, sizeof a);
				memset((char *)&c, 0x55, sizeof c);
				ra = i86dec(m, ip, &a);
				rc = i86decc(m, ip, &c);
				if (!decsame(&a, &c, ra, rc)) {
					printf("FAIL i86dec ip %04x prefixes %d op %02x next %02x: "
					    "asm len %d op %d fl %02x mod %d rm %d seg %d disp %04x imm %04x, "
					    "c len %d op %d fl %02x mod %d rm %d seg %d disp %04x imm %04x\n",
					    ip, n, op, g, a.len, a.op, a.fl, a.mod, a.rm, a.seg, a.disp, a.imm,
					    c.len, c.op, c.fl, c.mod, c.rm, c.seg, c.disp, c.imm);
					return 0;
				}
				nd++;
			}
		}
	}
	printf("PASS i86dec %lu decodes agree\n", nd);
	return 1;
}

/* ---- the run loops */

#define	NTEST	40000
#define	NCMP	16

static unsigned long seed = 1;

static unsigned
rnd()
{
	seed = seed * 1103515245L + 12345L;
	return (unsigned)(seed >> 12) & 0xffff;
}

/* opcodes of the classes the assembly runs */
static unsigned char hot[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0a, 0x0b,
	0x0c, 0x0d, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x18, 0x19,
	0x1a, 0x1b, 0x1c, 0x1d, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
	0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x30, 0x31, 0x32, 0x33,
	0x34, 0x35, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x80, 0x81,
	0x82, 0x83, 0x80, 0x81, 0x83, 0x83,
	0x88, 0x89, 0x8a, 0x8b, 0x88, 0x89, 0x8a, 0x8b, 0xc6, 0xc7,
	0xa0, 0xa1, 0xa2, 0xa3, 0xb0, 0xb3, 0xb4, 0xb7, 0xb8, 0xbc,
	0x84, 0x85, 0xa8, 0xa9, 0xf6, 0xf7, 0xf6, 0xf7,
	0x86, 0x87, 0x91, 0x94, 0x97,
	0x40, 0x43, 0x44, 0x47, 0x48, 0x49, 0x4c, 0x4f, 0xfe, 0xff,
	0xfe, 0xff, 0x50, 0x53, 0x54, 0x57, 0x58, 0x5b, 0x5c, 0x5f,
	0x8f, 0xe8, 0xc3, 0xc2, 0xe9, 0xeb, 0xe0, 0xe1, 0xe2, 0xe3,
	0x8d, 0x8d, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
	0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x70, 0x71,
	0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b,
	0x7c, 0x7d, 0x7e, 0x7f, 0xd0, 0xd1, 0xd2, 0xd3, 0xd0, 0xd1,
	0xd0, 0xd1, 0xd0, 0xd1, 0xd0, 0xd1, 0x90, 0x98, 0x99, 0x9e,
	0x9f, 0xf5, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0x9c, 0x9d
};
#define	NHOT	(sizeof hot / sizeof hot[0])

static i16 edge[] = {
	0, 1, 2, 0xffff, 0xfffe, 0xfffd, 0x7fff, 0x8000,
	0x007f, 0x0080, 0x00ff, 0x0100, 0x7ffe, 0x8001, 0xff7f, 0xff80
};

static struct i86 t, c, a;
static struct i86in inc, ina;
static char *ma, *mc;
static i16 soff[4];

static i16
rval()
{
	return (rnd() & 3) == 0 ? edge[rnd() & 15] : (i16)rnd();
}

static i16
put(p)
	i16 p;
{
	unsigned char b[10];
	int j, k, n = 0;

	k = rnd() & 31;
	if (k < 3)
		b[n++] = 0x26 + ((rnd() & 3) << 3);
	else if (k == 3)
		b[n++] = rnd() & 1 ? 0xf0 : 0xf3;
	k = rnd() & 31;
	if (k == 0) {
		/* a delay loop, or nearly one */
		j = rnd() & 7;
		if (rnd() & 1) {
			b[n++] = 0x48 + j;
			b[n++] = 0x75;
			b[n++] = rnd() & 3 ? 0xfd : rnd();
		} else {
			b[n++] = 0xfe + (rnd() & 1);
			b[n++] = 0xc8 + j;
			b[n++] = 0x75;
			b[n++] = rnd() & 3 ? 0xfc : rnd();
		}
	} else if (k == 1)
		b[n++] = rnd();
	else {
		b[n++] = hot[rnd() % NHOT];
		b[n] = rnd();
		if ((rnd() & 3) == 0)
			b[n] |= 0xc0;
		n++;
	}
	while (n < 10)
		b[n++] = rnd();
	for (j = 0; j < 10; j++) {
		mc[(i16)(p + j)] = b[j];
		ma[(i16)(p + j)] = b[j];
	}
	i86decc(mc, p, &inc);
	return (i16)(p + (inc.len ? inc.len : 1));
}

static void
gen()
{
	int i, n;
	i16 p;

	for (i = 0; i < 8; i++)
		t.r[i] = rval();
	if ((rnd() & 7) == 0)
		t.r[R_SP] = edge[rnd() & 7];
	for (i = 0; i < 4; i++) {
		t.sr[i] = rnd();
		t.so[i] = 0;
		soff[i] = i == S_CS ? 0 : rnd();
	}
	if ((rnd() & 15) == 0)
		t.so[(rnd() & 1) ? S_DS : (rnd() & 1) ? S_SS : S_ES] = rnd() | 1;
	t.ip = rnd() & 7 ? rnd() : 0xfff8 + (rnd() & 7);
	t.fl = rnd() & ~F_TF;
	if ((rnd() & 31) == 0)
		t.fl |= F_TF;
	t.lz = rnd() % 6;
	t.lw = rnd() & 1;
	t.lc = rnd() & 1;
	t.la = rval();
	t.lb = rval();
	t.lr = rval();
	if ((rnd() & 7) == 0)
		t.lb = t.lz == LZ_SUB ? t.la : 0;
	if (rnd() & 1)
		switch (t.lz) {
		case LZ_ADD: t.lr = t.la + t.lb + t.lc; break;
		case LZ_SUB: t.lr = t.la - t.lb - t.lc; break;
		case LZ_LOG: t.lc = 0; t.lr = t.la & t.lb; break;
		case LZ_INC: t.lc = 0; t.lb = 1; t.lr = t.la + 1; break;
		case LZ_DEC: t.lc = 0; t.lb = 1; t.lr = t.la - 1; break;
		}
	t.halt = rnd() & 1;
	t.fault = rnd() & 1;
	t.foff = rnd();
	t.fseg = rnd() & 3;
	t.wseg = rnd();
	t.wset = (rnd() & 7) == 0;
	n = rnd() & 7 ? 1 : 1 + (rnd() & 7);
	p = t.ip;
	for (i = 0; i < n; i++)
		p = put(p);
	i86nrun = n;
}

static void
bad(k, what, av, cv)
	unsigned long k;
	char *what;
	unsigned av, cv;
{
	int j;

	printf("FAIL i86run %s asm=%04x c=%04x test %lu ip=%04x:", what, av, cv, k, t.ip);
	for (j = 0; j < 8; j++)
		printf(" %02x", ma[(i16)(t.ip + j)] & 0xff);
	printf("\n");
}

#define	CHK(f, s)	if ((a.f) != (c.f)) { bad(k, s, (unsigned)(a.f), (unsigned)(c.f)); return 0; }

static int
same(k)
	unsigned long k;
{
	int i;

	for (i = 0; i < 8; i++)
		CHK(r[i], "r");
	for (i = 0; i < 4; i++) {
		CHK(sr[i], "sr");
		CHK(so[i], "so");
		if (a.sb[i] - ma != c.sb[i] - mc) {
			bad(k, "sb", i, i);
			return 0;
		}
	}
	CHK(ip, "ip");
	CHK(fl, "fl");
	CHK(lz, "lz");
	CHK(lw, "lw");
	CHK(lc, "lc");
	CHK(la, "la");
	CHK(lb, "lb");
	CHK(lr, "lr");
	CHK(halt, "halt");
	CHK(fault, "fault");
	CHK(foff, "foff");
	CHK(fseg, "fseg");
	CHK(wseg, "wseg");
	CHK(wset, "wset");
	return 1;
}

static int
irun()
{
	unsigned long k;
	int i, ra, rc, no;
	i16 nr, n;
	i32 ni, nf, ns;

	i86nseg = 0;
	for (k = 0; k < IMG; k++)
		ma[k] = mc[k] = rnd();
	for (k = 1; k <= NTEST; k++) {
		gen();
		nr = i86nrun;
		c = t;
		a = t;
		for (i = 0; i < 4; i++) {
			c.sb[i] = mc + soff[i];
			a.sb[i] = ma + soff[i];
		}
		i86ninsn = i86nflag = i86nskip = 0;
		i86intno = 0;
		rc = i86run(&c, &inc);
		ni = i86ninsn;
		nf = i86nflag;
		ns = i86nskip;
		no = i86intno;
		n = i86nrun;
		i86ninsn = i86nflag = i86nskip = 0;
		i86intno = 0;
		i86nrun = nr;
		ra = i86runa(&a, &ina);
		if (ra != rc)
			return bad(k, "return", ra, rc), 0;
		if (i86nrun != n)
			return bad(k, "i86nrun", i86nrun, n), 0;
		if (i86ninsn != ni)
			return bad(k, "i86ninsn", (unsigned)i86ninsn, (unsigned)ni), 0;
		if (i86nflag != nf)
			return bad(k, "i86nflag", (unsigned)i86nflag, (unsigned)nf), 0;
		if (i86nskip != ns || i86intno != no)
			return bad(k, "i86nskip/intno", (unsigned)i86nskip, (unsigned)ns), 0;
		if (!same(k))
			return 0;
		if (k % NCMP == 0 && memcmp(ma, mc, IMG) != 0) {
			for (i = 0; ma[i] == mc[i]; i++)
				;
			bad(k, "memory at", i, 0);
			return 0;
		}
	}
	printf("PASS i86run %d runs agree\n", NTEST);
	return 1;
}

static long
ms()
{
	struct timeval tv;

	gettimeofday(&tv, (struct timezone *)0);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* the BENCH86 loop: 40 x 65536 x (MOV BX,CX, ADD AX,BX, LOOP), then HLT */
static void
bench(mil)
	long mil;
{
	static unsigned char prg[] = {
		0xba, 40, 0, 0x31, 0xc9, 0x89, 0xcb, 0x01, 0xd8, 0xe2, 0xfa,
		0x4a, 0x75, 0xf5, 0xf4
	};
	long t0, k, n = mil * 1000000L;
	int pass, i, rc;

	for (pass = 0; pass < 2; pass++) {
		memset(ma, 0, 0x10000);
		memcpy(ma + 0x100, (char *)prg, sizeof prg);
		memset((char *)&a, 0, sizeof a);
		for (i = 0; i < 4; i++)
			a.sb[i] = ma;
		a.ip = 0x100;
		a.fl = F_ONES;
		i86nseg = 0;
		k = 0;
		t0 = ms();
		while (k < n) {
			i86nrun = 16384;
			rc = pass ? i86run(&a, &ina) : i86runa(&a, &ina);
			k += 16384 - i86nrun;
			if (rc != X_OK) {
				a.ip = 0x100;
				k++;
			}
		}
		t0 = ms() - t0;
		printf("INFO i86 %s run loop: %ld instructions in %ld ms\n", pass ? "C" : "68k", k, t0);
	}
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	if ((ma = malloc(IMG)) == 0 || (mc = malloc(IMG)) == 0)
		return 1;
	if (argc == 3 && strcmp(argv[1], "-t") == 0) {
		bench(atol(argv[2]));
		return 0;
	}
	return idec(ma) && irun() ? 0 : 1;
}
