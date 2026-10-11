/*
 * plane.c -- separate bitplanes drawn one plane at a time by the 1-bit
 * drawing library.
 */
#include "plane.h"
#include "../../drawops/drawops.h"

/* the library may change the block it is given: a fresh one a plane */
static void op1(struct do_op *o, long kind, unsigned char *a, long stride,
    long x, long y, long w, long h, long mode)
{
	o->kind = kind;
	o->depth = 1;
	o->dst = a;
	o->dstride = stride;
	o->x = x;
	o->y = y;
	o->w = w;
	o->h = h;
	o->mode = mode;
	o->fg = o->bg = 0;
	o->pmask = -1;
	o->src = 0;
	o->sstride = o->sx = o->sy = 0;
	o->pat = 0;
	o->style = 0;
	o->flags = 0;
}

void pl_fill(struct plbm *b, long x, long y, long w, long h,
    unsigned short const *pat, long fg, long bg, long mode)
{
	struct do_op o;
	unsigned char *a = b->base;
	long p;

	/* reverse transparent, solid: nothing to draw */
	if (mode == 4 && !pat)
		return;
	for (p = 0; p < b->planes; p++, a += b->pstep) {
		op1(&o, DO_FILL, a, b->stride, x, y, w, h, mode);
		o.fg = fg >> p & 1;
		o.bg = bg >> p & 1;
		o.pat = pat;
		/* both colours alike in this plane: the pattern doesn't matter */
		if (mode == 1 && o.fg == o.bg)
			o.pat = 0;
		do_draw(&o);
	}
}

/*
 * A glyph, at most 16 wide: each source row once, laid on every plane
 * through the word or longword of the plane row that holds it.  Every
 * row becomes (d & A) ^ X, X picked by the plane's fg and bg bits.
 */
static void narrow(struct plbm *b, long x, long y, long w, long h,
    unsigned char const *s, long sstride, long sx, long fg, long bg, long mode, long inv)
{
	unsigned char *row = b->base + y * b->stride + (x >> 4) * 2, *a, k[8];
	long sh = x & 15, wide = sh + w > 16, n = ((sx & 7) + w + 7) >> 3, j, p;
	unsigned long win = (0xffffffffUL << (32 - w)) >> sh, m, v, A, X[4];

	for (p = 0; p < b->planes; p++)
		k[p] = (fg >> p & 1) | (bg >> p & 1) << 1;
	s += sx >> 3;
	for (j = 0; j < h; j++, row += b->stride, s += sstride) {
		v = (unsigned long)s[0] << 24;
		if (n > 1)
			v |= (unsigned long)s[1] << 16;
		if (n > 2)
			v |= (unsigned long)s[2] << 8;
		v <<= sx & 7;
		if (inv)
			v = ~v;
		m = v >> sh & win;
		switch (mode) {
		case 1:
			A = ~win;
			X[0] = 0, X[1] = m, X[2] = win & ~m, X[3] = win;
			break;
		case 2:
			A = ~m;
			X[0] = X[2] = 0, X[1] = X[3] = m;
			break;
		case 3:
			A = ~0UL;
			X[0] = X[1] = X[2] = X[3] = m;
			break;
		default:
			m = win & ~m;
			A = ~m;
			X[0] = X[2] = 0, X[1] = X[3] = m;
		}
		a = row;
		if (wide)
			for (p = 0; p < b->planes; p++, a += b->pstep)
				*(unsigned long *)a = (*(unsigned long *)a & A) ^ X[k[p]];
		else
			for (p = 0; p < b->planes; p++, a += b->pstep)
				*(unsigned short *)a = (*(unsigned short *)a & A >> 16) ^ X[k[p]] >> 16;
	}
}

void pl_expand(struct plbm *b, long x, long y, long w, long h,
    void const *src, long sstride, long sx, long fg, long bg, long mode, long flags)
{
	struct do_op o;
	unsigned char *a = b->base;
	long p;

	if (w <= 16 && b->planes > 1 && !(((long)b->base | b->stride | b->pstep) & 1)) {
		narrow(b, x, y, w, h, src, sstride, sx, fg, bg, mode, flags & DOF_INVERT);
		return;
	}
	for (p = 0; p < b->planes; p++, a += b->pstep) {
		op1(&o, DO_EXPAND, a, b->stride, x, y, w, h, mode);
		o.fg = fg >> p & 1;
		o.bg = bg >> p & 1;
		o.src = src;
		o.sstride = sstride;
		o.sx = sx;
		o.flags = flags;
		if (mode == 1 && o.fg == o.bg) {
			o.kind = DO_FILL;
			o.pat = 0;
			o.flags = 0;
		}
		do_draw(&o);
	}
}

void pl_blit(struct plbm *s, long sx, long sy, struct plbm *d,
    long x, long y, long w, long h, long op)
{
	struct do_op o;
	unsigned char *a = d->base, *c = s->base;
	long p;

	for (p = 0; p < d->planes; p++, a += d->pstep, c += s->pstep) {
		op1(&o, DO_BLIT, a, d->stride, x, y, w, h, op);
		o.src = c;
		o.sstride = s->stride;
		o.sx = sx;
		o.sy = sy;
		do_draw(&o);
	}
}

/* Bresenham, every plane's bit at each step */
static void diagonal(struct plbm *b, long x1, long y1, long x2, long y2,
    long style, long fg, long bg, long mode)
{
	unsigned char *row = b->base + y1 * b->stride, *a, m, on[8], off[8];
	long dx = x2 > x1 ? x2 - x1 : x1 - x2, dy = y2 > y1 ? y2 - y1 : y1 - y2;
	long xs = x2 > x1 ? 1 : -1, ys = y2 > y1 ? 1 : -1, err = dx - dy, e2, p;
	unsigned short bit = 0x8000;

	/* style bits set and clear: 1 sets, 0 clears, 2 inverts, 3 leaves */
	for (p = 0; p < b->planes; p++) {
		on[p] = mode == 3 ? 2 : mode == 4 ? 3 : fg >> p & 1;
		off[p] = mode == 1 ? bg >> p & 1 : mode == 4 ? fg >> p & 1 : 3;
	}
	for (;;) {
		a = row + (x1 >> 3);
		m = 0x80 >> (x1 & 7);
		for (p = 0; p < b->planes; p++, a += b->pstep)
			switch (style & bit ? on[p] : off[p]) {
			case 0: *a &= ~m; break;
			case 1: *a |= m; break;
			case 2: *a ^= m; break;
			}
		if (!(bit >>= 1))
			bit = 0x8000;
		if (x1 == x2 && y1 == y2)
			break;
		e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			x1 += xs;
		}
		if (e2 < dx) {
			err += dx;
			y1 += ys;
			row += ys * b->stride;
		}
	}
}

void pl_line(struct plbm *b, long x1, long y1, long x2, long y2,
    long style, long fg, long bg, long mode)
{
	struct do_op o;
	unsigned char *a = b->base;
	long p;

	if (x1 != x2 && y1 != y2 && b->planes > 1) {
		diagonal(b, x1, y1, x2, y2, style, fg, bg, mode);
		return;
	}
	for (p = 0; p < b->planes; p++, a += b->pstep) {
		op1(&o, DO_LINE, a, b->stride, x1, y1, x2, y2, mode);
		o.fg = fg >> p & 1;
		o.bg = bg >> p & 1;
		o.style = style & 0xffff;
		if (mode == 1 && o.fg == o.bg)
			o.style = 0xffff;
		do_draw(&o);
	}
}

long pl_get(struct plbm *b, long x, long y)
{
	unsigned char *a = b->base + y * b->stride + (x >> 3), m = 0x80 >> (x & 7);
	long p, v = 0;

	for (p = 0; p < b->planes; p++, a += b->pstep)
		if (*a & m)
			v |= 1 << p;
	return v;
}

void pl_put(struct plbm *b, long x, long y, long v)
{
	unsigned char *a = b->base + y * b->stride + (x >> 3), m = 0x80 >> (x & 7);
	long p;

	for (p = 0; p < b->planes; p++, a += b->pstep, v >>= 1)
		*a = v & 1 ? *a | m : *a & ~m;
}

void pl_mshow(struct plbm *b, struct plsave *s, long x, long y, long w, long h,
    long sh, unsigned short const *md, long fg, long bg)
{
	unsigned char *row = b->base + y * b->stride + (x >> 3), *a, *sv = s->b;
	unsigned long keep, fgm, bgm, v, cut;
	long i, j, p, n = ((x & 7) + w + 7) >> 3;

	s->at = row;
	s->rows = h;
	s->bytes = n;
	cut = 0xffffUL << (16 - w) & 0xffff;
	for (j = 0; j < h; j++, row += b->stride, md += 2) {
		/* 24 bits from the pointer's first byte */
		bgm = ((unsigned long)(md[0] << sh) & cut) << (8 - (x & 7));
		fgm = ((unsigned long)(md[1] << sh) & cut) << (8 - (x & 7));
		bgm &= ~fgm;
		keep = ~(fgm | bgm);
		for (p = 0, a = row; p < b->planes; p++, a += b->pstep) {
			v = 0;
			for (i = 0; i < n; i++)
				v |= (unsigned long)(*sv++ = a[i]) << (16 - 8 * i);
			v = (v & keep) | (fg >> p & 1 ? fgm : 0) | (bg >> p & 1 ? bgm : 0);
			for (i = 0; i < n; i++)
				a[i] = v >> (16 - 8 * i);
		}
	}
}

void pl_mhide(struct plbm *b, struct plsave *s)
{
	unsigned char *row = s->at, *a, *sv = s->b;
	long i, j, p;

	if (!row)
		return;
	for (j = 0; j < s->rows; j++, row += b->stride)
		for (p = 0, a = row; p < b->planes; p++, a += b->pstep)
			for (i = 0; i < s->bytes; i++)
				a[i] = *sv++;
	s->at = 0;
}
