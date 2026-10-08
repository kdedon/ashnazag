/*
 * x86test.c -- the Win16 environment's x86 core against SingleStepTests'
 * 80386 real-mode suite (hardware traces of a 386EX, MOO format).
 *
 *	x86test [-v] [-l] [-k known] 80386.csv FILE.MOO.gz...
 *
 * Each test: load the initial registers and memory, run one instruction,
 * count the HALT after it, compare registers and memory.  Flags the CSV
 * marks undefined are not compared, also in a flags image an exception
 * pushed.  Prints one line per file and a total; exit 1 on a failure.
 * -v shows each difference, -l lists failing tests' hashes, -k names
 * a file of hashes not to count (x86known.txt).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86.h"

#define	MEMSIZE	0x1000000

static u8 *mem;
static struct desc ldt[1];
static int verbose;
static char *nmask[4096];
static u32 vmask[4096];
static int nmasks;

static u32
le32(p)
	u8 *p;
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* the CSV's flag mask for op[.ext] */
static u32
flagmask(key)
	char *key;
{
	int i;

	for (i = 0; i < nmasks; i++)
		if (strcmp(nmask[i], key) == 0)
			return vmask[i];
	return 0xffffffff;
}

static void
readcsv(path)
	char *path;
{
	FILE *fp = fopen(path, "r");
	char line[2048], *f[64], *p;
	int n, i;

	if (!fp) {
		perror(path);
		exit(2);
	}
	fgets(line, sizeof line, fp);
	while (fgets(line, sizeof line, fp) && nmasks < 4096) {
		for (n = 0, p = line; n < 64; n++) {
			f[n] = p;
			p = strchr(p, ',');
			if (!p)
				break;
			*p++ = 0;
		}
		if (n < 40)
			continue;
		nmask[nmasks] = malloc(16);
		if (*f[4])
			sprintf(nmask[nmasks], "%s.%s", f[0], f[4]);
		else
			sprintf(nmask[nmasks], "%s", f[0]);
		/* the undefined-flags mask is the field written 0xHHHH */
		vmask[nmasks] = 0xffffffff;
		for (i = 30; i <= n; i++)
			if (f[i][0] == '0' && f[i][1] == 'x' && strlen(f[i]) == 6)
				vmask[nmasks] = strtoul(f[i], 0, 16) | 0xffff0000;
		nmasks++;
	}
	fclose(fp);
}

/* chunk at p: type, length, payload */
#define	CTYPE(p, t)	(memcmp(p, t, 4) == 0)

static void
setregs(c, p)
	struct x86 *c;
	u8 *p;
{
	u32 mask = le32(p), v;
	int i;
	static int map[8] = { R_AX, R_BX, R_CX, R_DX, R_SI, R_DI, R_BP, R_SP };
	static int smap[6] = { S_CS, S_DS, S_ES, S_FS, S_GS, S_SS };

	p += 4;
	for (i = 0; i < 20; i++) {
		if (!(mask & 1 << i))
			continue;
		v = le32(p);
		p += 4;
		if (i == 0)
			c->cr0 = v;
		else if (i >= 2 && i <= 9)
			c->r[map[i - 2]] = v;
		else if (i >= 10 && i <= 15)
			x86_loadseg(c, smap[i - 10], v & 0xffff);
		else if (i == 16)
			c->eip = v;
		else if (i == 17)
			x86_setflags(c, v);
	}
}

static int
regval(c, i)
	struct x86 *c;
	int i;
{
	static int map[8] = { R_AX, R_BX, R_CX, R_DX, R_SI, R_DI, R_BP, R_SP };
	static int smap[6] = { S_CS, S_DS, S_ES, S_FS, S_GS, S_SS };

	if (i >= 2 && i <= 9)
		return c->r[map[i - 2]];
	if (i >= 10 && i <= 15)
		return c->s[smap[i - 10]].sel;
	if (i == 16)
		return c->eip;
	if (i == 17)
		return x86_flags(c);
	return 0;
}

static int nundef, nknown, listfail;
static char **known;
static int nkn;

static int
isknown(h)
	char *h;
{
	int i;

	for (i = 0; i < nkn; i++)
		if (strncmp(known[i], h, 40) == 0)
			return 1;
	return 0;
}

/* hashes of tests where the 386EX does what we choose not to */
static void
readknown(path)
	char *path;
{
	FILE *fp = fopen(path, "r");
	char line[256];

	if (!fp) {
		perror(path);
		exit(2);
	}
	while (fgets(line, sizeof line, fp))
		if (strlen(line) >= 40 && line[0] != '#') {
			known = realloc(known, (nkn + 1) * sizeof *known);
			known[nkn] = malloc(41);
			memcpy(known[nkn], line, 40);
			known[nkn++][40] = 0;
		}
	fclose(fp);
}

/*
 * Results Intel leaves undefined that the 386EX's differ from ours in:
 * 1 skip the test, else 0 with *fmask narrowed.  Shifts and rotates
 * of a byte or word by a count past its width leave CF and OF
 * undefined; SHLD/SHRD of a word by more than 16 leave everything.
 */
static int
undefined(t, end, fmask)
	u8 *t, *end;
	u32 *fmask;
{
	u8 *p, *b = 0;
	u32 l, n, i, cl = 0, mask;
	int o32 = 0, op, m, cnt;

	for (p = t + 4; p < end; p += 8 + l) {
		l = le32(p + 4);
		if (CTYPE(p, "BYTS")) {
			b = p + 12;
			n = le32(p + 8);
		} else if (CTYPE(p, "INIT")) {
			u8 *q;

			for (q = p + 8; q < p + 8 + l; q += 8 + le32(q + 4))
				if (CTYPE(q, "RG32")) {
					mask = le32(q + 8);
					cl = le32(q + 12 + 4 * 4);	/* cr0 cr3 eax ebx ecx */
					(void)mask;
				}
		}
	}
	if (!b)
		return 0;
	for (i = 0; i < n; i++) {
		op = b[i];
		if (op == 0x66)
			o32 = 1;
		else if (op == 0x67 || op == 0xf0 || op == 0xf2 || op == 0xf3 || op == 0x26 ||
		    op == 0x2e || op == 0x36 || op == 0x3e || op == 0x64 || op == 0x65)
			;
		else
			break;
	}
	if (i + 1 >= n)
		return 0;
	op = b[i];
	m = b[i + 1];
	switch (op) {
	case 0xe4: case 0xe5: case 0xec: case 0xed:
	case 0x6c: case 0x6d:
		return 1;		/* IN: what the bus had */
	case 0xd2: case 0xd3: case 0xc0: case 0xc1:
		if ((op == 0xd3 || op == 0xc1) && o32)
			return 0;
		if (op < 0xd0) {
			/* the count is the last byte before the HALT */
			cnt = b[n - 2];
		} else
			cnt = cl & 0xff;
		cnt &= 31;
		if (cnt >= ((op & 1) ? 16 : 8))
			*fmask &= ~(F_CF | F_OF);
		return 0;
	case 0x0f:
		op = m;
		if ((op == 0xa4 || op == 0xa5 || op == 0xac || op == 0xad) && !o32) {
			cnt = (op & 1) ? cl & 31 : b[n - 2] & 31;
			if (cnt > 16)
				return 1;
		}
		return 0;
	}
	return 0;
}

static char *rname[20] = { "cr0", "cr3", "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp",
	"esp", "cs", "ds", "es", "fs", "gs", "ss", "eip", "eflags", "dr6", "dr7" };

static int
runtest(c, t, n, fmask, name, file)
	struct x86 *c;
	u8 *t;
	u32 n, fmask;
	char *name, *file;
{
	u8 *p, *q, *end = t + n, *fin = 0, *ini = 0;
	u32 l, i, cnt, a, mask, v, want, excaddr = ~0;
	int bad = 0, regs;
	char tname[128], hash[41];
	struct x86 init;

	tname[0] = 0;
	for (p = t + 4; p < end; p += 8 + l) {
		l = le32(p + 4);
		if (CTYPE(p, "INIT"))
			ini = p + 8;
		else if (CTYPE(p, "FINA"))
			fin = p + 8;
		else if (CTYPE(p, "HASH"))
			for (i = 0; i < 20; i++)
				sprintf(hash + 2 * i, "%02x", p[8 + i]);
		else if (CTYPE(p, "EXCP"))
			excaddr = le32(p + 9);
		else if (CTYPE(p, "NAME")) {
			cnt = le32(p + 8);
			if (cnt > 127)
				cnt = 127;
			memcpy(tname, p + 12, cnt);
			tname[cnt] = 0;
		}
	}
	if (!ini || !fin)
		return 0;
	switch (undefined(t, end, &fmask)) {
	case 1:
		nundef++;
		return 0;
	}
	x86_init(c, mem, MEMSIZE, ldt);
	/* registers, then memory */
	l = le32(ini - 4);
	for (p = ini; p < ini + l; p += 8 + le32(p + 4))
		if (CTYPE(p, "RG32"))
			setregs(c, p + 8);
	for (p = ini; p < ini + l; p += 8 + le32(p + 4))
		if (CTYPE(p, "RAM ")) {
			cnt = le32(p + 8);
			for (i = 0, q = p + 12; i < cnt; i++, q += 5)
				mem[le32(q) & (MEMSIZE - 1)] = q[4];
		}
	memcpy(&init, c, sizeof init);
	x86_step(c);
	/* the HALT: past the code segment's limit it faults instead */
	if (c->eip > c->s[S_CS].limit)
		x86_step(c);
	c->eip += 1;
	l = le32(fin - 4);
	for (p = fin; p < fin + l; p += 8 + le32(p + 4)) {
		if (CTYPE(p, "RG32")) {
			mask = le32(p + 8);
			q = p + 12;
			regs = 0;
			for (i = 0; i < 20; i++) {
				if (!(mask & 1 << i))
					continue;
				want = le32(q);
				q += 4;
				regs |= 1 << i;
				if (i < 2 || i > 17)
					continue;
				v = regval(c, i);
				if (i >= 10 && i <= 15)
					v &= 0xffff, want &= 0xffff;
				if (i == 17)
					v &= fmask, want &= fmask;
				if (v != want) {
					if (verbose)
						printf("  %s %s: %s %08x, want %08x\n", file, tname, rname[i], v, want);
					bad++;
				}
			}
		} else if (CTYPE(p, "RAM ")) {
			cnt = le32(p + 8);
			for (i = 0, q = p + 12; i < cnt; i++, q += 5) {
				a = le32(q);
				v = mem[a & (MEMSIZE - 1)];
				want = q[4];
				if (a == excaddr || a == excaddr + 1) {
					u32 m = a == excaddr ? fmask & 0xff : fmask >> 8 & 0xff;

					v &= m;
					want &= m;
				}
				if (v != want) {
					if (verbose)
						printf("  %s %s: [%06x] %02x, want %02x\n", file, tname, a, v, want);
					bad++;
				}
			}
		}
	}
	if (bad && isknown(hash)) {
		nknown++;
		return 0;
	}
	if (bad && listfail)
		printf("%s %s %s\n", hash, file, tname);
	if (bad && verbose)
		printf("    was eax %08x ebx %08x ecx %08x edx %08x esp %08x ebp %08x esi %08x edi %08x fl %08x\n",
		    init.r[R_AX], init.r[R_BX], init.r[R_CX], init.r[R_DX], init.r[R_SP], init.r[R_BP],
		    init.r[R_SI], init.r[R_DI], x86_flags(&init));
	return bad != 0;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct x86 cpu;
	int i, fails = 0, total = 0, ffail, ftotal, j;
	char cmd[1024], key[64], *b, *s;
	u8 *buf;
	u32 n, cap, l, fmask;
	FILE *fp;

	for (;;) {
		if (argc > 1 && strcmp(argv[1], "-v") == 0)
			verbose = 1;
		else if (argc > 1 && strcmp(argv[1], "-l") == 0)
			listfail = 1;
		else if (argc > 2 && strcmp(argv[1], "-k") == 0) {
			readknown(argv[2]);
			argv++;
			argc--;
		} else
			break;
		argv++;
		argc--;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: x86test [-v] [-l] [-k known] 80386.csv FILE.MOO.gz...\n");
		return 2;
	}
	readcsv(argv[1]);
	mem = calloc(1, MEMSIZE);
	cap = 1 << 20;
	buf = malloc(cap);
	for (i = 2; i < argc; i++) {
		b = strrchr(argv[i], '/');
		b = b ? b + 1 : argv[i];
		strncpy(key, b, sizeof key - 1);
		key[sizeof key - 1] = 0;
		if ((s = strstr(key, ".MOO")) != 0)
			*s = 0;
		s = key;
		while ((s[0] == '6' && (s[1] == '6' || s[1] == '7')) && strlen(s) > 2)
			s += 2;
		fmask = flagmask(s);
		sprintf(cmd, "gzip -dc '%s'", argv[i]);
		fp = popen(cmd, "r");
		n = 0;
		while ((l = fread(buf + n, 1, cap - n, fp)) > 0) {
			n += l;
			if (n == cap)
				buf = realloc(buf, cap *= 2);
		}
		pclose(fp);
		ffail = ftotal = 0;
		for (j = 0; j + 8 <= n; j += 8 + le32(buf + j + 4)) {
			if (!CTYPE(buf + j, "TEST"))
				continue;
			ftotal++;
			ffail += runtest(&cpu, buf + j + 8, le32(buf + j + 4), fmask, s, b);
		}
		printf("%-14s %5d tests %5d failed\n", b, ftotal, ffail);
		fflush(stdout);
		fails += ffail;
		total += ftotal;
	}
	printf("total %d tests, %d failed, %d undefined not run, %d known differences\n",
	    total, fails, nundef, nknown);
	return fails != 0;
}
