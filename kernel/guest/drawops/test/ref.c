/*
 * ref.c -- drawops pixel by pixel, the reference for the tests.
 */
#include "../drawops.h"

typedef unsigned char u8;

static long get(struct do_op *o, u8 *b, long st, long x, long y)
{
	u8 *p = b + y * st;

	if (o->depth == 1)
		return p[x >> 3] >> (7 - (x & 7)) & 1;
	return p[x];
}

static void put(struct do_op *o, u8 *b, long st, long x, long y, long v)
{
	u8 *p = b + y * st;

	if (o->depth == 1)
		p[x >> 3] = (p[x >> 3] & ~(0x80 >> (x & 7))) | (v & 1) << (7 - (x & 7));
	else
		p[x] = v;
}

static long bymode(struct do_op *o, long bit, long d, long pm)
{
	long n, all = o->depth == 1 ? 1 : 255;

	switch (o->mode) {
	case 1: n = bit ? o->fg : o->bg; break;
	case 2: n = bit ? o->fg : d; break;
	case 3: n = bit ? ~d : d; break;
	default: n = bit ? d : o->fg; break;
	}
	pm &= all;
	return ((d & ~pm) | (n & pm)) & all;
}

static long logic(long op, long s, long d)
{
	long r = 0, k;

	for (k = 0; k < 8; k++) {
		long sb = s >> k & 1, db = d >> k & 1;
		long bit = sb ? (db ? 0 : 1) : (db ? 2 : 3);
		r |= (op >> bit & 1) << k;
	}
	return r;
}

static u8 tmp[1 << 16];

void ref(struct do_op *o)
{
	u8 *d = o->dst, *s = (u8 *)o->src;
	long i, j, bit;

	switch (o->kind) {
	case DO_FILL:
		for (j = 0; j < o->h; j++)
			for (i = 0; i < o->w; i++) {
				long x = o->x + i, y = o->y + j;
				bit = o->pat ? o->pat[y & 15] >> (15 - (x & 15)) & 1 : 1;
				put(o, d, o->dstride, x, y,
				    bymode(o, bit, get(o, d, o->dstride, x, y), o->pmask));
			}
		break;
	case DO_EXPAND:
		for (j = 0; j < o->h; j++)
			for (i = 0; i < o->w; i++) {
				long sx = o->sx + i, x = o->x + i, y = o->y + j;
				bit = s[(o->sy + j) * o->sstride + (sx >> 3)] >> (7 - (sx & 7)) & 1;
				bit ^= o->flags & DOF_INVERT;
				put(o, d, o->dstride, x, y,
				    bymode(o, bit, get(o, d, o->dstride, x, y), o->pmask));
			}
		break;
	case DO_BLIT:
		for (j = 0; j < o->h; j++)
			for (i = 0; i < o->w; i++)
				tmp[j * o->w + i] = get(o, s, o->sstride, o->sx + i, o->sy + j);
		for (j = 0; j < o->h; j++)
			for (i = 0; i < o->w; i++) {
				long x = o->x + i, y = o->y + j;
				long v = logic(o->mode, tmp[j * o->w + i], get(o, d, o->dstride, x, y));
				put(o, d, o->dstride, x, y, o->depth == 1 ? v & 1 : v);
			}
		break;
	case DO_LINE: {
		long x1 = o->x, y1 = o->y, x2 = o->w, y2 = o->h, dx, dy, sx, sy, err, e2;
		unsigned short m = 0x8000;

		dx = x2 > x1 ? x2 - x1 : x1 - x2;
		dy = y2 > y1 ? y2 - y1 : y1 - y2;
		sx = x2 > x1 ? 1 : -1;
		sy = y2 > y1 ? 1 : -1;
		err = dx - dy;
		for (;;) {
			put(o, d, o->dstride, x1, y1,
			    bymode(o, (o->style & m) != 0, get(o, d, o->dstride, x1, y1), -1));
			if (!(m >>= 1))
				m = 0x8000;
			if (x1 == x2 && y1 == y2)
				break;
			e2 = 2 * err;
			if (e2 > -dy) { err -= dy; x1 += sx; }
			if (e2 < dx) { err += dx; y1 += sy; }
		}
		break;
	}
	}
}
