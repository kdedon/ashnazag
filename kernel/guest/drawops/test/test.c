/*
 * test.c -- every drawops operation against ref.c, bit for bit, on
 * random rectangles, strides, alignments, patterns and modes.
 *
 *   test [cpu] [count]
 */
#include "../drawops.h"

typedef unsigned char u8;
long sys_write(long fd, void const *b, long n);
void ref(struct do_op *o);

#define	BW	1024
#define	BH	48
#define	GUARD	64
#define	SIZE	(GUARD + BW * BH + GUARD)

static u8 ba[SIZE] __attribute__((aligned(16))), bb[SIZE] __attribute__((aligned(16)));
static u8 sa[SIZE] __attribute__((aligned(16)));
static unsigned short pat[16];
static unsigned long seed = 1;

void *memcpy(void *d, void const *s, unsigned long n)
{
	u8 *a = d;
	u8 const *b = s;

	while (n--)
		*a++ = *b++;
	return d;
}

static unsigned long rnd(void)
{
	seed = seed * 1103515245 + 12345;
	return seed >> 8;
}

static long r(long n)
{
	return n > 0 ? (long)(rnd() % n) : 0;
}

static void out(char const *s)
{
	long n = 0;

	while (s[n])
		n++;
	sys_write(1, s, n);
}

static void num(long v)
{
	char b[12];
	int i = 11;

	b[i] = 0;
	if (v < 0) {
		out("-");
		v = -v;
	}
	do
		b[--i] = '0' + v % 10;
	while (v /= 10);
	out(b + i);
}

static long nhook;
static long hook(struct do_op *o)
{
	(void)o;
	nhook++;
	return 1;
}

static char const *const names[] = { "", "fill", "expand", "blit", "line" };
static long fails[5], runs[5];

static void one(long kind, long depth)
{
	struct do_op o, p;
	long i, maxw, st, off, same;

	for (i = 0; i < SIZE; i++)
		ba[i] = bb[i] = rnd(), sa[i] = rnd();
	for (i = 0; i < 16; i++)
		pat[i] = rnd();
	maxw = depth == 1 ? 900 : 200;
	if (r(4) == 0)
		maxw = 40;
	st = depth == 1 ? 1 + (maxw + 40 + 7) / 8 + r(9) : maxw + 40 + r(9);
	if (r(2))
		st = (st + 15) & ~15;
	off = GUARD + r(16);
	o.kind = kind;
	o.depth = depth;
	o.dst = ba + off;
	o.dstride = st;
	o.w = 1 + r(maxw);
	o.h = 1 + r(BH - 8);
	o.x = r(maxw + 40 - o.w);
	o.y = r(BH - o.h);
	o.mode = 1 + r(4);
	o.fg = rnd();
	o.bg = rnd();
	o.pmask = r(3) ? -1 : (long)rnd();
	o.src = sa + GUARD + r(16);
	o.sstride = st;
	o.sx = r(64);
	o.sy = 0;
	o.pat = r(3) ? pat : 0;
	o.style = r(2) ? 0xffff : rnd() & 0xffff;
	o.flags = r(2) ? DOF_INVERT : 0;
	if (kind == DO_EXPAND) {
		o.depth = depth;
		o.sstride = (o.sx + o.w + 7) / 8 + r(5);
		if (o.sstride * o.h > BW * BH)
			o.h = BW * BH / o.sstride;
	}
	if (kind == DO_BLIT) {
		o.mode = r(3) ? 3 : r(16);
		o.pmask = -1;
		o.sx = r(maxw + 40 - o.w);
		o.sy = r(BH - o.h);
		same = r(2);
		if (same)
			o.src = o.dst;
		else if (r(2))
			o.src = sa + off;
		/* the same place in a 16-byte line: MOVE16 rows */
		if (r(3) == 0 && o.x + 16 < maxw + 40 - o.w)
			o.sx = o.x + (r(2) ? 16 : 0);
	}
	if (kind == DO_LINE) {
		o.pmask = -1;
		o.w = r(maxw + 40);
		o.h = r(BH);
		if (r(3) == 0)
			o.h = o.y;
		else if (r(3) == 0)
			o.w = o.x;
	}
	p = o;
	p.dst = bb + off;
	if (kind == DO_BLIT && o.src == o.dst)
		p.src = p.dst;
	do_draw(&o);
	ref(&p);
	runs[kind]++;
	for (i = 0; i < SIZE; i++)
		if (ba[i] != bb[i]) {
			if (fails[kind]++ < 5) {
				out(names[kind]); out(" depth "); num(depth);
				out(" x "); num(o.x); out(" y "); num(o.y);
				out(" w "); num(o.w); out(" h "); num(o.h);
				out(" st "); num(st); out(" off "); num(off & 15);
				out(" mode "); num(o.mode); out(" sx "); num(o.sx);
				out(" sy "); num(o.sy); out(" pat "); num(o.pat != 0);
				out(" pm "); num(o.pmask & 255); out(" fl "); num(o.flags);
				out(" same "); num(o.src == o.dst);
				out(": byte "); num(i - off); out("\n");
			}
			break;
		}
}

int main(int argc, char **argv, char **envp)
{
	long cpu = 30, count = 3000, i, k, d, bad = 0, only = 0;
	struct do_op o;

	(void)envp;
	if (argc > 1)
		for (cpu = 0, i = 0; argv[1][i]; i++)
			cpu = cpu * 10 + argv[1][i] - '0';
	if (argc > 2)
		for (count = 0, i = 0; argv[2][i]; i++)
			count = count * 10 + argv[2][i] - '0';
	if (argc > 3)
		only = argv[3][0] - '0';
	do_init(cpu);
	for (i = 0; i < count; i++)
		for (k = DO_FILL; k <= DO_LINE; k++)
			for (d = 1; d <= 8; d += 7)
				if (!only || only == k)
					one(k, d);
	if (only)
		count = 0;
	/* the executor gets what reaches its limit, and only that */
	do_hook = hook;
	do_limit[DO_FILL] = 100;
	o.kind = DO_FILL; o.depth = 8; o.dst = ba + GUARD; o.dstride = 64;
	o.x = o.y = 0; o.w = 10; o.h = 10; o.mode = 1; o.fg = 1; o.bg = 0;
	o.pmask = -1; o.pat = 0; o.flags = 0;
	ba[GUARD + 64 * 8] = ba[GUARD + 64 * 9] = 0;
	do_draw(&o);
	o.h = 9;
	do_draw(&o);
	do_hook = 0;
	if (nhook != 1 || ba[GUARD + 64 * 8] != 1 || ba[GUARD + 64 * 9] != 0) {
		out("hook: wrong\n");
		bad++;
	}
	for (k = 1; k <= 4; k++) {
		out(names[k]); out(": "); num(runs[k] - fails[k]); out(" of "); num(runs[k]); out(" match\n");
		bad += fails[k];
	}
	out(bad ? "FAIL\n" : "ok\n");
	return bad != 0;
}
