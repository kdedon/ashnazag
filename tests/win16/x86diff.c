/*
 * x86diff.c -- the Win16 x86 core's 68k run loop against the C
 * reference, instruction by instruction, on the target.
 *
 *	x86diff [-v] [-n cases] [-s seed] [corpus]
 *	x86diff -t N	time N million instructions of a loop, both cores
 *
 * Random states, in protected mode over an LDT of every kind of
 * descriptor and in real mode, running from random bytes, mostly opcodes
 * the run loop handles: one instruction, or in protected mode up to six
 * at a stretch (the run loop's own segment state carried between them);
 * then a far-pointer word sum's loop whole, and the initial states in
 * corpus (mkcorpus.py, from the 80386 suite).
 * Registers, flags (as made from either core's lazy record), segments,
 * counters, faults and memory must match.
 * Prints the totals and how many instructions went to the reference;
 * exit 1 on a difference.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "x86.h"

#define	MEM	0x18000		/* random states */
#define	CMEM	0x110000	/* the corpus: real mode, up to FFFF:FFFF */
#define	NLDT	32

extern int x86_useasm;
extern u32 x86_nslow;

static struct x86 ca, cb, c0;
static u8 *ma, *mb;
static struct desc ldt[LDTSIZE];
static int verbose, ndiff, nslow1, nrun;
static u32 seed = 1;
static struct frec {
	int n, e, cnt;
} fa, fb;

static u32
rnd()
{
	seed = seed * 1103515245 + 12345;
	return seed >> 8 & 0xffff | (seed << 8 & 0xffff0000);
}

static u32
rn(n)
	u32 n;
{
	return rnd() % n;
}

static int
onfault(c, n, e)
	struct x86 *c;
	int n, e;
{
	struct frec *f = (struct frec *)c->user;

	f->n = n;
	f->e = e;
	f->cnt++;
	return 0;
}

static int
onint(c, n)
	struct x86 *c;
	int n;
{
	return 0;
}

/* the opcodes the run loop handles, and a few it does not */
static u8 ops[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d,
	0x40, 0x43, 0x44, 0x46, 0x48, 0x49, 0x4c, 0x4f, 0x50, 0x53, 0x54, 0x56, 0x58, 0x5b, 0x5c, 0x5f,
	0x68, 0x6a, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b,
	0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x90, 0x91, 0x95, 0x97, 0x98, 0x99, 0x9a, 0x9b,
	0x9c, 0x9d, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab,
	0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb4, 0xb7, 0xb8, 0xbc, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xd0, 0xd1, 0xd2, 0xd3,
	0xe0, 0xe1, 0xe2, 0xe3, 0xe8, 0xe9, 0xea, 0xeb, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
	0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
	0x0f, 0x27, 0x60, 0x61, 0x69, 0x8f, 0x9e, 0x9f, 0xcd, 0xd4, 0xd7, 0xf4
};

/* a descriptor of a random kind */
static void
mkdesc(d)
	struct desc *d;
{
	static u8 accs[] = { 0x9b, 0x9a, 0x98, 0x93, 0x93, 0x93, 0x92, 0x91, 0x90, 0x97, 0x96, 0x13, 0x82, 0 };
	static u32 lims[] = { 0xffff, 0xffff, 0xfff, 0x1ff, 0x40, 0x10, 0 };

	d->d_acc = accs[rn(sizeof accs)];
	d->d_limit = lims[rn(sizeof lims / sizeof lims[0])];
	if (rn(4) == 0)
		d->d_limit = rn(0x10000);
	d->d_base = rn(MEM);
	if (rn(8) == 0)
		d->d_base = MEM - rn(0x100);
	d->d_flags = 0;
	d->d_owner = 0;
}

static u32
anysel(acc)
	int acc;
{
	int i, n;

	for (n = 0; n < 64; n++) {
		i = 1 + rn(NLDT - 1);
		if (!acc || ldt[i].d_acc == acc || (acc == 0x93 && (ldt[i].d_acc & 0x9a) == 0x92))
			return SEL(i);
	}
	return SEL(1);
}

/* a random state in A, its instruction at CS:IP; B the same */
static void
mkstate()
{
	int i, n, prot = rn(5) != 0;
	u32 ip, a, lim, v;

	if (rn(64) == 0) {
		for (i = 1; i < NLDT; i++)
			mkdesc(&ldt[i]);
		ldt[1].d_acc = 0x9b;		/* a code, a stack and a data segment always */
		ldt[1].d_limit = 0xffff;
		ldt[2].d_acc = 0x93;
		ldt[2].d_limit = 0xffff;
		ldt[3].d_acc = 0x93;
	}
	x86_init(&ca, ma, MEM, ldt);
	ca.fault = onfault;
	ca.intr = onint;
	ca.prot = prot;
	ca.cr0 = prot;
	if (prot) {
		x86_loadseg(&ca, S_CS, rn(4) ? SEL(1) : anysel(0x9a));
		x86_loadseg(&ca, S_SS, rn(4) ? SEL(2) : anysel(0x93));
		x86_loadseg(&ca, S_DS, rn(3) ? SEL(3) : rn(8) ? anysel(0) : rn(4));
		x86_loadseg(&ca, S_ES, rn(3) ? SEL(2) : rn(8) ? anysel(0) : rn(4));
		if (ca.s[S_CS].acc == 0 || !(ca.s[S_CS].acc & D_CODE))
			x86_loadseg(&ca, S_CS, SEL(1));
	} else
		for (i = 0; i < 4; i++)
			x86_loadseg(&ca, i, rn(MEM >> 4));
	for (i = 0; i < 8; i++) {
		v = rnd();
		switch (rn(4)) {
		case 0: v = v & 0xffff0000 | rn(0x40); break;
		case 1: v = v & 0xffff0000 | (ca.s[S_DS].limit - rn(4)) & 0xffff; break;
		}
		ca.r[i] = v;
	}
	lim = ca.s[S_SS].limit;
	ca.r[R_SP] = ca.r[R_SP] & 0xffff0000 | (rn(8) ? (lim > 8 ? lim - rn(lim < 0x100 ? lim : 0x100) : 0) : rn(0x10000)) & 0xfffe;
	if (rn(4) == 0)
		ca.r[R_SP] |= 1;
	ca.r[R_CX] = ca.r[R_CX] & 0xffff0000 | (rn(4) ? rn(24) : rnd() & 0xffff);
	x86_setflags(&ca, rnd() & (0xffff & ~F_TF) | 2);
	if (rn(3)) {		/* a pending record, as an instruction leaves it */
		u32 d = rnd(), s = rnd(), m;

		ca.lf_sz = rn(3);
		m = ca.lf_sz == 0 ? 0xff : ca.lf_sz == 1 ? 0xffff : 0xffffffff;
		d &= m;
		s &= m;
		switch (ca.lf_op = 1 + rn(5)) {
		case LF_ADD: ca.lf_res = (d + s + rn(2)) & m; break;
		case LF_SUB: ca.lf_res = (d - s - rn(2)) & m; break;
		case LF_LOGIC: ca.lf_res = (rn(2) ? d & s : d ^ s) & m; break;
		case LF_INC: s = 1; ca.lf_res = (d + 1) & m; break;
		default: s = 1; ca.lf_res = (d - 1) & m; break;
		}
		ca.lf_dst = d;
		ca.lf_src = s;
	}
	lim = ca.s[S_CS].limit;
	ip = rn(8) ? rn(lim > 0x20 ? lim - 0x20 : 1) : lim - rn(0x18);
	ca.eip = ip & 0xffff;
	a = ca.s[S_CS].base + ca.eip;
	n = 0;
	if (rn(8) == 0)
		ma[a + n++] = "\x26\x2e\x36\x3e"[rn(4)];
	if (rn(16) == 0)
		ma[a + n++] = rn(2) ? 0xf3 : 0xf2;
	if (rn(64) == 0)
		ma[a + n++] = 0x66;
	ma[a + n++] = ops[rn(sizeof ops)];
	if (rn(16) == 0)
		ma[a + n - 1] = rnd();
	i = n;
	while (n < 12)
		ma[a + n++] = rn(3) ? rnd() : rn(8);
	if ((ma[a + i - 1] == 0x9a || ma[a + i - 1] == 0xea) && rn(4)) {	/* a far pointer that works */
		v = prot ? (rn(2) ? SEL(1) : anysel(0x9a)) : rn(MEM >> 4);
		ma[a + i] = rnd();
		ma[a + i + 1] = rn(4);
		ma[a + i + 2] = v;
		ma[a + i + 3] = v >> 8;
	}
	nrun = prot && rn(2) ? 2 + rn(5) : 1;
	cb = ca;
	ca.user = (void *)&fa;
	cb.user = (void *)&fb;
	cb.mem = mb;
	memcpy(mb, ma, MEM);
}

static void
show(what, a, b)
	char *what;
	u32 a, b;
{
	if (verbose || ndiff < 20)
		printf("  %s: asm %08x, C %08x\n", what, a, b);
}

/* 0 when A and B agree */
static int
same(code, size)
	u8 *code;
	u32 size;
{
	struct x86 fa_, fb_;
	int i, bad = 0;
	u32 j;
	char name[32];

	fa_ = ca;
	fb_ = cb;
	for (i = 0; i < 8; i++)
		if (ca.r[i] != cb.r[i]) {
			sprintf(name, "r%d", i);
			show(name, ca.r[i], cb.r[i]);
			bad++;
		}
#define	F(f)	if (ca.f != cb.f) { show(#f, (u32)ca.f, (u32)cb.f); bad++; }
	F(eip) F(icount) F(stop) F(halted) F(prot) F(cr0)
	for (i = 0; i < 6; i++) {
		F(s[i].base) F(s[i].limit) F(s[i].sel) F(s[i].acc) F(s[i].big)
	}
	if (x86_flags(&fa_) != x86_flags(&fb_)) {
		show("flags", fa_.fl, fb_.fl);
		bad++;
	}
	if (fa.cnt != fb.cnt || fa.n != fb.n || fa.e != fb.e) {
		show("fault", fa.cnt << 16 | fa.n << 8 | fa.e, fb.cnt << 16 | fb.n << 8 | fb.e);
		bad++;
	}
	for (j = 0; j < size; j++)
		if (ma[j] != mb[j]) {
			show("memory at", j, ma[j] << 8 | mb[j]);
			bad++;
			break;
		}
	if (bad) {
		ndiff++;
		if (verbose || ndiff < 20) {
			printf("  after");
			for (i = 0; i < 12; i++)
				printf(" %02x", code[i]);
			printf("\n  from");
			for (i = 0; i < 8; i++)
				printf(" %08x", c0.r[i]);
			printf("\n  ip %08x fl %08x prot %d lf %d/%d %x %x %x\n", c0.eip, c0.fl, c0.prot, c0.lf_op, c0.lf_sz,
			    c0.lf_dst, c0.lf_src, c0.lf_res);
			for (i = 0; i < 6; i++)
				printf("  s%d %04x base %06x limit %05x acc %02x\n", i, c0.s[i].sel, c0.s[i].base, c0.s[i].limit,
				    c0.s[i].acc);
			printf("  asm (%d instructions, %d to the reference):", nrun, nslow1);
			for (i = 0; i < 8; i++)
				printf(" %08x", ca.r[i]);
			printf("\n  C:");
			for (i = 0; i < 8; i++)
				printf(" %08x", cb.r[i]);
			printf("\n");
		}
	}
	return bad;
}

/* both cores one instruction from the state in A and B */
static int
run1(size)
	u32 size;
{
	u8 code[12];
	u32 a = ca.s[S_CS].base + ca.eip;
	int i;

	for (i = 0; i < 12; i++)
		code[i] = a + i < size ? ma[a + i] : 0;
	c0 = ca;
	memset((char *)&fa, 0, sizeof fa);
	memset((char *)&fb, 0, sizeof fb);
	x86_useasm = 1;
	nslow1 = x86_nslow;
	x86_stepn(&ca, nrun);
	nslow1 = x86_nslow - nslow1;
	x86_useasm = 0;
	for (i = 0; i < nrun; i++)
		if (x86_step(&cb))
			break;
	if (same(code, size)) {
		memcpy(mb, ma, size);
		return 1;
	}
	return 0;
}

static u32
be32(p)
	u8 *p;
{
	return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* the corpus: per case cr0, AX CX DX BX SP BP SI DI, ES CS SS DS FS GS,
 * IP, flags, then RAM bytes (address, value) and the addresses the
 * instruction may write, each list counted (big-endian) */
static int
corpus(path, nc)
	char *path;
	int *nc;
{
	FILE *fp = fopen(path, "rb");
	u8 h[64], b[5];
	int bad = 0, i, n;
	u32 a;
	static u32 *touched;
	int nt;

	if (!fp) {
		perror(path);
		return 1;
	}
	touched = (u32 *)malloc(65536 * sizeof(u32));
	while (fread(h, 1, 4 + 32 + 12 + 8 + 2, fp) == 58) {
		x86_init(&ca, ma, CMEM, ldt);
		ca.fault = onfault;
		ca.intr = onint;
		ca.cr0 = be32(h);
		for (i = 0; i < 8; i++)
			ca.r[i] = be32(h + 4 + 4 * i);
		for (i = 0; i < 6; i++)
			x86_loadseg(&ca, i, h[36 + 2 * i] << 8 | h[37 + 2 * i]);
		ca.eip = be32(h + 48);
		x86_setflags(&ca, be32(h + 52));
		n = h[56] << 8 | h[57];
		nt = 0;
		for (i = 0; i < n && fread(b, 1, 5, fp) == 5; i++) {
			a = be32(b) % CMEM;
			ma[a] = b[4];
			touched[nt++ & 0xffff] = a;
		}
		if (fread(b, 1, 2, fp) != 2)
			break;
		n = b[0] << 8 | b[1];
		for (i = 0; i < n && fread(b, 1, 4, fp) == 4; i++)
			touched[nt++ & 0xffff] = be32(b) % CMEM;
		cb = ca;
		ca.user = (void *)&fa;
		cb.user = (void *)&fb;
		cb.mem = mb;
		nrun = 1;
		for (i = 0; i < nt && i < 65536; i++)
			mb[touched[i]] = ma[touched[i]];
		bad += run1(0);
		for (i = 0; i < nt && i < 65536; i++)
			if (ma[touched[i]] != mb[touched[i]]) {
				show("memory at", touched[i], ma[touched[i]] << 8 | mb[touched[i]]);
				bad++;
				ndiff++;
				break;
			}
		(*nc)++;
	}
	fclose(fp);
	return bad;
}

static long
ms()
{
	struct timeval tv;

	gettimeofday(&tv, (struct timezone *)0);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/*
 * A loop of compiled code's kind: loads, stores, ALU, INC, PUSH/POP,
 * CMP and Jcc, LOOP, a call, LES and a JMP back; in protected mode.
 */
static void
bench(mil)
	long mil;
{
	static u8 prg[] = {
		0xb9, 0x00, 0x40,		/* mov cx, 4000h */
		0x8b, 0x00,			/* mov ax, [bx+si] */
		0x01, 0xd0,			/* add ax, dx */
		0x89, 0x01,			/* mov [bx+di], ax */
		0x46,				/* inc si */
		0x81, 0xe6, 0xfe, 0x0f,		/* and si, 0ffeh */
		0x50,				/* push ax */
		0x5a,				/* pop dx */
		0x39, 0xce,			/* cmp si, cx */
		0x75, 0x00,			/* jne $+2 */
		0xe8, 0x07, 0x00,		/* call sub */
		0xc4, 0x7e, 0x02,		/* les di, [bp+2] */
		0xe2, 0xe7,			/* loop (mov ax) */
		0xeb, 0xe2,			/* jmp start */
		0x8b, 0x46, 0x04,		/* sub: mov ax, [bp+4] */
		0xc3				/* ret */
	};
	long t0, done, n = mil * 1000000L;
	int pass, k;

	for (pass = 0; pass < 2; pass++) {
		memset(ma, 0, MEM);
		memcpy(ma, (char *)prg, sizeof prg);
		ldt[1].d_base = 0;
		ldt[1].d_limit = 0xfff;
		ldt[1].d_acc = 0x9b;
		ldt[2].d_base = 0x1000;
		ldt[2].d_limit = 0xffff;
		ldt[2].d_acc = 0x93;
		ma[0x1000 + 4] = SEL(2);	/* LES's selector */
		x86_init(&ca, ma, MEM, ldt);
		ca.fault = onfault;
		ca.prot = 1;
		x86_loadseg(&ca, S_CS, SEL(1));
		x86_loadseg(&ca, S_DS, SEL(2));
		x86_loadseg(&ca, S_ES, SEL(2));
		x86_loadseg(&ca, S_SS, SEL(2));
		ca.r[R_SP] = 0xfff0;
		ca.r[R_BX] = 0x2000;
		ca.r[R_DI] = 0x10;
		ca.user = (void *)&fa;
		x86_useasm = !pass;
		t0 = ms();
		for (done = 0; done < n && !ca.stop; done += k) {
			k = n - done > 100000 ? 100000 : n - done;
			x86_stepn(&ca, k);
		}
		t0 = ms() - t0;
		printf("%s: %ld instructions in %ld ms%s\n", pass ? "C" : "68k", done, t0, ca.stop ? " (stopped)" : "");
	}
}

/*
 * A far-pointer word sum (the run loop takes it whole) on both cores,
 * to its HLT: random registers, lengths and an ES that may end inside
 * it or hold the pointer itself.
 */
static int
sumloop(n)
	int n;
{
	static u8 prg[] = {
		0xeb, 0x0b,			/* jmp L2 */
		0xc4, 0x5e, 0xfc,		/* L1: les bx, [bp-4] */
		0x26, 0x03, 0x37,		/* add si, es:[bx] */
		0x4f,				/* dec di */
		0x83, 0x46, 0xfc, 0x02,		/* add word [bp-4], 2 */
		0x0b, 0xff,			/* L2: or di, di */
		0x75, 0xf1,			/* jne L1 */
		0xf4				/* hlt */
	};
	static int regs[] = { 0, 1, 2, 6, 7 };
	int i, k, r, c, bad = 0;
	u32 p, s0 = x86_nslow;

	for (k = 0; k < MEM; k++)
		ma[k] = rnd();
	for (i = 0; i < n; i++) {
		r = regs[rn(5)];
		do
			c = regs[rn(5)];
		while (c == r);
		prg[7] = r << 3 | 7;
		prg[8] = 0x48 + c;
		prg[14] = 0xc0 | c << 3 | c;
		memcpy(ma, (char *)prg, sizeof prg);
		ldt[1].d_base = 0;
		ldt[1].d_limit = 0xfff;
		ldt[1].d_acc = 0x9b;
		ldt[2].d_base = 0x1000;
		ldt[2].d_limit = 0xffff;
		ldt[2].d_acc = 0x93;
		ldt[3].d_base = 0x8000 + rn(0x8000);
		ldt[3].d_limit = rn(4) ? 0xffff : rn(0x1000);
		ldt[3].d_acc = rn(4) ? 0x93 : 0x91;
		x86_init(&ca, ma, MEM, ldt);
		ca.fault = onfault;
		ca.intr = onint;
		ca.prot = 1;
		x86_loadseg(&ca, S_CS, SEL(1));
		x86_loadseg(&ca, S_DS, SEL(2));
		x86_loadseg(&ca, S_ES, SEL(2));
		x86_loadseg(&ca, S_SS, SEL(2));
		for (k = 0; k < 8; k++)
			ca.r[k] = rnd() & 0xffff;
		ca.r[R_BP] = 0x100 + rn(0xf000);
		ca.r[R_SP] = 0xfff0;
		ca.r[c] = rn(0x400);
		p = 0x1000 + ca.r[R_BP] - 4;
		k = rn(8) ? 3 : 2;		/* 2: the pointer inside ES */
		ma[p] = rn(0x10000) & 0xfe;
		ma[p + 1] = k == 2 ? (ca.r[R_BP] - 4 - rn(0x40)) >> 8 : rn(0x100);
		ma[p + 2] = SEL(k);
		ma[p + 3] = 0;
		ca.user = (void *)&fa;
		cb = ca;
		cb.user = (void *)&fb;
		cb.mem = mb;
		memcpy(mb, ma, MEM);
		c0 = ca;
		memset((char *)&fa, 0, sizeof fa);
		memset((char *)&fb, 0, sizeof fb);
		x86_useasm = 1;
		x86_run(&ca);
		x86_useasm = 0;
		x86_run(&cb);
		bad += same(prg, MEM) != 0;
	}
	printf("word sums: %d cases, %d differ, %ld to the reference\n", n, bad, (long)(x86_nslow - s0));
	return bad;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	int n = 20000, i, bad = 0, nc = 0;
	u32 s0;

	for (;;) {
		if (argc > 1 && strcmp(argv[1], "-v") == 0)
			verbose = 1;
		else if (argc > 2 && strcmp(argv[1], "-n") == 0) {
			n = atoi(argv[2]);
			argv++;
			argc--;
		} else if (argc > 2 && strcmp(argv[1], "-s") == 0) {
			seed = atoi(argv[2]);
			argv++;
			argc--;
		} else
			break;
		argv++;
		argc--;
	}
	ma = (u8 *)malloc(CMEM);
	mb = (u8 *)malloc(CMEM);
	if (argc == 3 && strcmp(argv[1], "-t") == 0) {
		bench(atol(argv[2]));
		return 0;
	}
	for (i = 0; i < MEM; i++)
		ma[i] = rnd();
	for (i = 0; i < MEM; i += 2)		/* selectors for LDS, RETF and the like */
		if (rn(4) == 0) {
			ma[i] = SEL(1 + rn(NLDT - 1));
			ma[i + 1] = 0;
		}
	for (i = 1; i < NLDT; i++)
		mkdesc(&ldt[i]);
	s0 = x86_nslow;
	for (i = 0; i < n; i++) {
		mkstate();
		bad += run1(MEM);
	}
	printf("random: %d cases, %d differ, %ld to the reference\n", n, bad, (long)(x86_nslow - s0));
	bad += sumloop(n / 100);
	if (argc > 1) {
		memset(ma, 0, CMEM);
		memset(mb, 0, CMEM);
		s0 = x86_nslow;
		i = corpus(argv[1], &nc);
		printf("corpus: %d cases, %d differ, %ld to the reference\n", nc, i, (long)(x86_nslow - s0));
		bad += i;
	}
	return bad != 0;
}
