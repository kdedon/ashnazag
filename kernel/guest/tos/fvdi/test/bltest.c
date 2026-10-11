/*
 * bltest.c -- the blitter registers hwblit.c sets up, run on a model of
 * the blitter (sometimes cut short) and checked against the drawing
 * reference, bit for bit.
 *
 *   cc -O2 -o bltest bltest.c ../../../drawops/test/ref.c; ./bltest [count]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	HOSTTEST
#include "../hwblit.c"

long (*do_hook)(struct do_op *);
long do_limit[5];
void ref(struct do_op *o);

/* the blitter, word by word */
long bcall(struct bregs *r)
{
	unsigned long in = 0, sv, dv, o;
	unsigned char *sa = (unsigned char *)r->sa, *da = (unsigned char *)r->da;
	int first, last, xc, line = r->ctl & 15;
	long stop = rand() % 4 ? 0 : rand() % (r->yn + 1);	/* a signal cuts it short */

#define	W(a)	((unsigned)(a)[0] << 8 | (a)[1])
	for (; r->yn > stop; r->yn--) {
		for (xc = r->xn, first = 1; xc; xc--, first = 0) {
			last = xc == 1;
			if (r->hop & 2) {
				if (r->sxi >= 0) {
					if (first && (r->skew & FXSR)) {
						in = W(sa);
						sa += r->sxi;
					}
					in <<= 16;
					if (last && (r->skew & NFSR))
						sa -= r->sxi;
					else {
						in |= W(sa);
						if (!last)
							sa += r->sxi;
					}
				} else {
					if (first && (r->skew & FXSR)) {
						in = W(sa);
						sa += r->sxi;
					} else
						in >>= 16;
					if (last && (r->skew & NFSR))
						sa -= r->sxi;
					else {
						in |= (unsigned long)W(sa) << 16;
						if (!last)
							sa += r->sxi;
					}
				}
				sv = in >> (r->skew & 15) & 0xffff;
			} else
				sv = 0xffff;
			if (r->hop & 1)
				sv &= r->ht[line];
			dv = W(da);
			o = 0;
			for (int b = 0; b < 16; b++) {
				int s = sv >> b & 1, d = dv >> b & 1;
				o |= (unsigned long)(r->op >> (s ? (d ? 0 : 1) : (d ? 2 : 3)) & 1) << b;
			}
			o = first ? (o & r->em[0]) | (dv & ~r->em[0]) :
			    last ? (o & r->em[2]) | (dv & ~r->em[2]) : (o & r->em[1]) | (dv & ~r->em[1]);
			da[0] = o >> 8;
			da[1] = o;
			if (!last)
				da += r->dxi;
		}
		line = (line + (r->dyi >= 0 ? 1 : 15)) & 15;
		sa += r->syi;
		da += r->dyi;
	}
	if (!r->yn)
		r->ctl &= 0x7f;
	return 0;
}

#define	ST	24		/* bytes a row */
#define	H	40
static unsigned char a[2][ST * H + 64], b[2][ST * H + 64];
static unsigned short pat[16];

int main(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 20000, i, j, bad = 0, offered = 0;
	struct do_op o, p;
	int k, t;

	srand(1);
	for (i = 0; i < n; i++) {
		for (k = 0; k < 2; k++)
			for (j = 0; j < (long)sizeof a[k]; j++)
				a[k][j] = b[k][j] = rand();
		for (j = 0; j < 16; j++)
			pat[j] = rand();
		memset(&o, 0, sizeof o);
		o.kind = rand() % 2 ? DO_FILL : DO_BLIT;
		o.depth = 1;
		o.pmask = 1;
		t = rand() % 2;
		o.dst = a[t] + 32;
		o.dstride = ST;
		o.w = 1 + rand() % (ST * 8);
		o.h = 1 + rand() % H;
		o.x = rand() % (ST * 8 - o.w + 1);
		o.y = rand() % (H - o.h + 1);
		if (o.kind == DO_FILL) {
			o.mode = 1 + rand() % 4;
			o.fg = rand() & 1;
			o.bg = rand() & 1;
			o.pat = rand() % 3 ? pat : 0;
		} else {
			o.mode = rand() % 16;
			o.src = a[rand() % 3 ? t : !t] + 32;
			o.sstride = ST;
			o.sx = rand() % (ST * 8 - o.w + 1);
			o.sy = rand() % (H - o.h + 1);
			/* overlaps of all kinds: near the destination */
			if (o.src == o.dst && rand() % 2) {
				o.sx = o.x + rand() % 33 - 16;
				o.sy = o.y + rand() % 5 - 2;
				if (o.sx < 0 || o.sx + o.w > ST * 8 || o.sy < 0 || o.sy + o.h > H)
					o.sx = o.x, o.sy = o.y;
			}
		}
		p = o;
		p.dst = b[t] + 32;
		if (o.src)
			p.src = (unsigned char *)o.src - a[o.src == a[t] + 32 ? t : !t] +
			    b[o.src == a[t] + 32 ? t : !t];
		ref(&p);
		if (hw_hook(&o))
			offered++;
		else
			ref(&o);	/* the rest */
		if (memcmp(a, b, sizeof a)) {
			if (bad++ < 10)
				printf("bad: kind %ld mode %ld x %ld y %ld w %ld h %ld sx %ld sy %ld fg %ld bg %ld pat %d same %d\n",
				    o.kind, o.mode, o.x, o.y, o.w, o.h, o.sx, o.sy, o.fg, o.bg, o.pat != 0,
				    o.src == o.dst);
		}
	}
	printf("%ld operations, %ld whole on the blitter, %ld bad\n", n, offered, bad);
	return bad != 0;
}
