/*
 * pltest.c -- the frame buffer drivers' drawing, bit for bit, against
 * the pixel reference: each random operation runs on an 8-bit chunky
 * bitmap (drawops), on 1, 2, 3, 4 or 8 separate bitplanes (plane.c)
 * and on the reference.  The planes must hold the reference's pixels
 * and leave every other byte alone.  Then the pointer: drawn over
 * random planes, it must show its colours and restore them exactly.
 *
 *   pltest [cpu] [count]
 */
#include "../../../drawops/drawops.h"
#include "../plane.h"

typedef unsigned char u8;
long sys_write(long fd, void const *b, long n);
void ref(struct do_op *o);

#define	H	40
#define	MAXST	64		/* bytes a plane row */
#define	GAP	24		/* guard bytes around and between planes */
#define	CSIZE	(MAXST * 8 * H)
#define	PSIZE	(GAP + 8 * (MAXST * H + GAP))

static u8 ca[CSIZE], cb[CSIZE], cs[CSIZE], src1[MAXST * H];
static u8 pa[PSIZE], pe[PSIZE], ps[PSIZE];
static unsigned short pat[16], mdata[32];
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

/* chunky pixels into the planes of pl, other bits untouched */
static void toplanes(u8 const *c, long cst, struct plbm *pl, long w)
{
	long x, y;

	for (y = 0; y < H; y++)
		for (x = 0; x < w; x++)
			pl_put(pl, x, y, c[y * cst + x]);
}

static char const *const names[] = { "", "fill", "expand", "blit", "line", "pointer" };
static long fails[6], runs[6];

static void fail(long kind, long n, char const *what, long at)
{
	if (fails[kind]++ < 5) {
		out(names[kind]); out(" planes "); num(n);
		out(": "); out(what); out(" at "); num(at); out("\n");
	}
}

static void one(long kind, long n)
{
	struct do_op o, p, q;
	struct plbm pl, sp;
	long i, st, w, maxw, mask = (1 << n) - 1;

	st = 8 + r(MAXST - 7);
	if (r(2))
		st &= ~3;
	w = st * 8;
	pl.base = pa + GAP;
	pl.stride = st;
	pl.pstep = st * H + r(GAP / 2) * 2;
	pl.planes = n;
	sp = pl;
	sp.base = ps + GAP;
	for (i = 0; i < PSIZE; i++)
		pa[i] = rnd(), ps[i] = rnd();
	for (i = 0; i < CSIZE; i++)
		ca[i] = rnd() & mask, cs[i] = rnd() & mask;
	for (i = 0; i < MAXST * H; i++)
		src1[i] = rnd();
	toplanes(ca, w, &pl, w);
	toplanes(cs, w, &sp, w);
	for (i = 0; i < CSIZE; i++)
		cb[i] = ca[i];
	for (i = 0; i < PSIZE; i++)
		pe[i] = pa[i];
	for (i = 0; i < 16; i++)
		pat[i] = rnd();
	maxw = w > 300 ? 300 : w;
	if (r(3) == 0)
		maxw = 20;
	o.kind = kind;
	o.depth = 8;
	o.dst = ca;
	o.dstride = w;
	o.w = 1 + r(maxw);
	o.h = 1 + r(H);
	o.x = r(w - o.w + 1);
	o.y = r(H - o.h + 1);
	o.mode = 1 + r(4);
	o.fg = rnd() & mask;
	o.bg = rnd() & mask;
	o.pmask = -1;
	o.src = 0;
	o.sstride = o.sx = o.sy = 0;
	o.pat = r(3) ? pat : 0;
	o.style = r(2) ? 0xffff : rnd() & 0xffff;
	o.flags = 0;
	if (kind == DO_EXPAND) {
		o.src = src1;
		o.sx = r(64);
		o.sstride = (o.sx + o.w + 7) / 8 + r(5);
		if (o.sstride * o.h > MAXST * H)
			o.h = MAXST * H / o.sstride;
		o.flags = r(4) ? 0 : DOF_INVERT;
		o.pat = 0;
	}
	if (kind == DO_BLIT) {
		o.mode = r(3) ? 3 : r(16);
		o.sx = r(w - o.w + 1);
		o.sy = r(H - o.h + 1);
		o.src = r(2) ? ca : cs;
		o.sstride = w;
	}
	if (kind == DO_LINE) {
		o.w = r(w);
		o.h = r(H);
		if (r(3) == 0)
			o.h = o.y;
		else if (r(3) == 0)
			o.w = o.x;
	}
	q = p = o;
	p.dst = cb;
	if (o.src == ca)
		p.src = cb;
	do_draw(&o);
	ref(&p);
	o = q;
	switch (kind) {
	case DO_FILL:
		pl_fill(&pl, o.x, o.y, o.w, o.h, o.pat, o.fg, o.bg, o.mode);
		break;
	case DO_EXPAND:
		pl_expand(&pl, o.x, o.y, o.w, o.h, o.src, o.sstride, o.sx, o.fg, o.bg, o.mode, o.flags);
		break;
	case DO_BLIT:
		pl_blit(o.src == ca ? &pl : &sp, o.sx, o.sy, &pl, o.x, o.y, o.w, o.h, o.mode);
		break;
	case DO_LINE:
		pl_line(&pl, o.x, o.y, o.w, o.h, o.style, o.fg, o.bg, o.mode);
		break;
	}
	runs[kind]++;
	/* the 8-bit result, then the planes with the reference's pixels */
	for (i = 0; i < CSIZE; i++)
		if (n == 8 && ca[i] != cb[i]) {
			fail(kind, 0, "chunky byte", i);
			return;
		}
	for (i = 0; i < CSIZE; i++)
		cb[i] &= mask;
	pl.base = pe + GAP;
	toplanes(cb, w, &pl, w);
	for (i = 0; i < PSIZE; i++)
		if (pa[i] != pe[i]) {
			fail(kind, n, "plane byte", i);
			return;
		}
}

static void pointer(long n)
{
	struct plbm pl;
	static struct plsave sv;
	long i, j, st = 8 + r(MAXST - 7), x, y, w, h, sh, fg, bg, mask = (1 << n) - 1;

	pl.base = pa + GAP;
	pl.stride = st;
	pl.pstep = st * H + GAP;
	pl.planes = n;
	for (i = 0; i < PSIZE; i++)
		pa[i] = pe[i] = rnd();
	for (i = 0; i < 32; i++)
		mdata[i] = rnd();
	fg = rnd() & mask;
	bg = rnd() & mask;
	sh = r(3) ? 0 : r(16);
	w = 16 - sh;
	h = 1 + r(16);
	x = r(st * 8 - w + 1);
	if (r(3) == 0)
		w -= r(w);
	y = r(H - h + 1);
	runs[5]++;
	pl_mshow(&pl, &sv, x, y, w, h, sh, mdata, fg, bg);
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			unsigned short bm = mdata[2 * j] << sh, dm = mdata[2 * j + 1] << sh;
			long want, bit = 0x8000 >> i;
			struct plbm e = pl;

			e.base = pe + GAP;
			want = dm & bit ? fg : bm & bit ? bg : pl_get(&e, x + i, y + j);
			if (pl_get(&pl, x + i, y + j) != want) {
				fail(5, n, "pixel", j * 16 + i);
				return;
			}
		}
	pl_mhide(&pl, &sv);
	for (i = 0; i < PSIZE; i++)
		if (pa[i] != pe[i]) {
			fail(5, n, "restored byte", i);
			return;
		}
}

int main(int argc, char **argv, char **envp)
{
	static long const depths[] = { 1, 2, 3, 4, 8 };
	long cpu = 30, count = 500, i, k, d, bad = 0;

	(void)envp;
	if (argc > 1)
		for (cpu = 0, i = 0; argv[1][i]; i++)
			cpu = cpu * 10 + argv[1][i] - '0';
	if (argc > 2)
		for (count = 0, i = 0; argv[2][i]; i++)
			count = count * 10 + argv[2][i] - '0';
	do_init(cpu);
	for (i = 0; i < count; i++)
		for (d = 0; d < 5; d++) {
			for (k = DO_FILL; k <= DO_LINE; k++)
				one(k, depths[d]);
			pointer(depths[d]);
		}
	for (k = 1; k <= 5; k++) {
		out(names[k]); out(": "); num(runs[k] - fails[k]); out(" of "); num(runs[k]); out(" match\n");
		bad += fails[k];
	}
	out(bad ? "FAIL\n" : "ok\n");
	return bad != 0;
}
