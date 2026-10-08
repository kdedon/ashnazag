/*
 * ttf.c -- TrueType, which Windows 3.1's GDI renders itself: the .TTF
 * files the user's Windows brings (Arial, Times New Roman, Courier New,
 * Symbol, Wingdings), named in WIN.INI's [fonts] through their .FOT
 * headers.  Each face at each size becomes a bitmap font like the .FON
 * ones, its advances computed at once and its glyphs scan converted
 * when first drawn: the quadratic outlines filled by the nonzero rule
 * at pixel centres, with dropout control so thin stems and bars stay,
 * as the 3.1 rasterizer fills.  No hinting.  Integer arithmetic only
 * (26.6 fixed point), for a 68k without an FPU.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "win.h"
#include "font.h"

#define	B16(p)	((unsigned)(p)[0] << 8 | (p)[1])
#define	S16(p)	((int)(short)B16(p))
#define	B32(p)	((u32)B16(p) << 16 | B16((p) + 2))

#define	FIRST	0x20
#define	NCH	(256 - FIRST)

struct ttfile {
	struct ttfile *next;
	char	*path;
	u8	*d;			/* the file, while used */
	long	n;
	char	face[64];
	int	bold, italic, weight, family, fixed, symbol;
	int	upem, locfmt, nglyph, nhm;
	int	asc, desc, avgw, maxw;
	u32	cmap, loca, glyf, hmtx, glyflen;	/* offsets in d */
};

struct ttsize {
	struct ttsize *next;
	struct ttfile *tf;
	int	ppem, shear;
	long	xn, xd, yn, yd;		/* font units to 26.6 pixels: * n / d */
	struct bfont bf;
	unsigned short adv[NCH];
	unsigned char w[NCH];
	struct ttglyph g[NCH];
	char	done[NCH];
};

static struct ttfile *files;
static struct ttsize *sizes;

/* ---- the file ---- */

static u8 *
table(d, n, tag, len)
	u8 *d;
	long n;
	char *tag;
	u32 *len;
{
	int i, nt;

	if (n < 12)
		return 0;
	nt = B16(d + 4);
	for (i = 0; i < nt && 12 + 16 * i + 16 <= n; i++) {
		u8 *e = d + 12 + 16 * i;
		u32 off = B32(e + 8), l = B32(e + 12);

		if (memcmp(e, tag, 4) == 0 && off + l <= (u32)n) {
			if (len)
				*len = l;
			return d + off;
		}
	}
	return 0;
}

static int
load(tf)
	struct ttfile *tf;
{
	FILE *fp;

	if (tf->d)
		return 1;
	if ((fp = fopen(tf->path, "rb")) == 0)
		return 0;
	fseek(fp, 0L, 2);
	tf->n = ftell(fp);
	fseek(fp, 0L, 0);
	if (tf->n < 12 || tf->n > 8L << 20 || (tf->d = (u8 *)malloc(tf->n)) == 0) {
		fclose(fp);
		return 0;
	}
	if (fread(tf->d, 1, tf->n, fp) != tf->n) {
		free(tf->d);
		tf->d = 0;
		fclose(fp);
		return 0;
	}
	fclose(fp);
	return 1;
}

/* the family name: nameID 1, Microsoft's (UTF-16) or Apple's */
static void
famname(d, n, out)
	u8 *d;
	long n;
	char *out;
{
	u8 *t = table(d, n, "name", (u32 *)0), *r, *s;
	int i, cnt, j, k, best = -1;

	out[0] = 0;
	if (!t)
		return;
	cnt = B16(t + 2);
	s = t + B16(t + 4);
	for (i = 0; i < cnt; i++) {
		r = t + 6 + 12 * i;
		if (B16(r + 6) != 1 || r + 12 > d + n)
			continue;
		if (B16(r) == 3 && (best < 0 || B16(r + 4) == 0x409)) {
			for (j = k = 0; j + 1 < B16(r + 8) && k < 63; j += 2)
				out[k++] = s[B16(r + 10) + j + 1];
			out[k] = 0;
			best = 3;
		} else if (B16(r) == 1 && best < 0) {
			k = B16(r + 8) > 63 ? 63 : B16(r + 8);
			memcpy(out, s + B16(r + 10), k);
			out[k] = 0;
			best = 1;
		}
	}
}

/* a .TTF joins the faces; 1 if it is one */
int
ttf_add(path)
	char *path;
{
	struct ttfile *tf, *o;
	u8 *d, *t, *c;
	u32 len;
	int i, nsub;

	for (o = files; o; o = o->next)
		if (strcmp(o->path, path) == 0)
			return 1;
	tf = (struct ttfile *)calloc(1, sizeof *tf);
	tf->path = strdup(path);
	if (!load(tf))
		goto bad;
	d = tf->d;
	if (!(t = table(d, tf->n, "head", (u32 *)0)))
		goto bad;
	tf->upem = B16(t + 18);
	tf->locfmt = S16(t + 50);
	tf->bold = B16(t + 44) & 1;
	tf->italic = (B16(t + 44) & 2) != 0;
	if (!(t = table(d, tf->n, "maxp", (u32 *)0)))
		goto bad;
	tf->nglyph = B16(t + 4);
	if (!(t = table(d, tf->n, "hhea", (u32 *)0)))
		goto bad;
	tf->nhm = B16(t + 34);
	tf->maxw = B16(t + 10);
	tf->asc = S16(t + 4);
	tf->desc = -S16(t + 6);
	if ((t = table(d, tf->n, "OS/2", &len)) != 0 && len >= 78) {
		tf->avgw = S16(t + 2);
		tf->weight = B16(t + 4);
		tf->asc = B16(t + 74);
		tf->desc = B16(t + 76);
		switch (t[30]) {
		case 1: case 2: case 3: case 4: case 5: case 7:
			tf->family = 0x10;	/* FF_ROMAN */
			break;
		case 10:
			tf->family = 0x40;	/* FF_SCRIPT */
			break;
		case 12:
			tf->family = 0x50;	/* FF_DECORATIVE */
			break;
		default:
			tf->family = 0x20;	/* FF_SWISS */
		}
	} else {
		tf->weight = tf->bold ? 700 : 400;
		tf->family = 0x20;
	}
	if ((t = table(d, tf->n, "post", &len)) != 0 && len >= 16 && B32(t + 12))
		tf->fixed = 1, tf->family = 0x30;	/* FF_MODERN */
	if (!(t = table(d, tf->n, "cmap", (u32 *)0)))
		goto bad;
	nsub = B16(t + 2);
	for (i = 0; i < nsub; i++) {
		c = t + 4 + 8 * i;
		if (B16(c) == 3 && (B16(c + 2) == 1 || B16(c + 2) == 0) && B16(t + B32(c + 4)) == 4) {
			tf->cmap = (t - d) + B32(c + 4);
			tf->symbol = B16(c + 2) == 0;
			break;
		}
		if (B16(c) == 1 && B16(c + 2) == 0 && B16(t + B32(c + 4)) == 0 && !tf->cmap)
			tf->cmap = (t - d) + B32(c + 4);
	}
	if (tf->symbol)
		tf->family = 0x50;
	if (!tf->cmap || !(t = table(d, tf->n, "loca", (u32 *)0)))
		goto bad;
	tf->loca = t - d;
	if (!(t = table(d, tf->n, "glyf", &tf->glyflen)))
		goto bad;
	tf->glyf = t - d;
	if (!(t = table(d, tf->n, "hmtx", (u32 *)0)))
		goto bad;
	tf->hmtx = t - d;
	famname(d, tf->n, tf->face);
	if (!tf->face[0] || tf->upem <= 0 || tf->nhm <= 0)
		goto bad;
	if (tf->avgw <= 0)
		tf->avgw = tf->maxw / 2;
	/* kept until used */
	free(tf->d);
	tf->d = 0;
	tf->next = files;
	files = tf;
	if (w16_debug)
		w16_log("startwin: TrueType %s%s%s (%s)\n", tf->face, tf->bold ? " bold" : "",
		    tf->italic ? " italic" : "", path);
	return 1;
bad:
	if (tf->d)
		free(tf->d);
	free(tf->path);
	free((char *)tf);
	return 0;
}

/* Windows' ANSI characters 0x80-0x9f in Unicode (the rest are Latin-1) */
static unsigned short ansi[32] = {
	0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
	0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
};

static int
cmap4(t, u)
	u8 *t;
	unsigned u;
{
	int seg2 = B16(t + 6), i;
	u8 *end = t + 14, *start = end + seg2 + 2, *delta = start + seg2, *ro = delta + seg2;

	for (i = 0; i < seg2; i += 2) {
		if (B16(end + i) < u)
			continue;
		if (B16(start + i) > u)
			return 0;
		if (B16(ro + i) == 0)
			return (u + B16(delta + i)) & 0xffff;
		{
			u8 *g = ro + i + B16(ro + i) + 2 * (u - B16(start + i));
			unsigned v = B16(g);

			return v ? (v + B16(delta + i)) & 0xffff : 0;
		}
	}
	return 0;
}

/* the glyph for an ANSI character */
static int
gindex(tf, c)
	struct ttfile *tf;
	int c;
{
	u8 *t = tf->d + tf->cmap;
	unsigned u;
	int g;

	if (B16(t) == 0)
		return c < 256 ? t[6 + c] : 0;
	if (tf->symbol) {
		g = cmap4(t, 0xf000 | c);
		return g ? g : cmap4(t, c);
	}
	u = c >= 0x80 && c < 0xa0 ? ansi[c - 0x80] : c;
	return u ? cmap4(t, u) : 0;
}

static int
advance(tf, g)
	struct ttfile *tf;
	int g;
{
	u8 *h = tf->d + tf->hmtx;

	return B16(h + 4 * (g < tf->nhm ? g : tf->nhm - 1));
}

/* ---- outlines ---- */

#define	MAXPT	2048
#define	MAXCT	256

struct outline {
	long	x[MAXPT], y[MAXPT];	/* font units, y up (scaled by composites) */
	char	on[MAXPT];
	int	end[MAXCT];
	int	npt, nct;
};

/* glyph g into o, through the 2.14 matrix m and offset dx, dy */
static void
glyph(tf, g, o, m, dx, dy, depth)
	struct ttfile *tf;
	int g, depth;
	struct outline *o;
	long *m, dx, dy;
{
	u8 *d = tf->d, *p, *lim;
	u32 off, next;
	int nc, i, n, fl, k, base;
	long v;

	if (g >= tf->nglyph || depth > 8)
		return;
	if (tf->locfmt) {
		off = B32(d + tf->loca + 4 * g);
		next = B32(d + tf->loca + 4 * g + 4);
	} else {
		off = 2 * (u32)B16(d + tf->loca + 2 * g);
		next = 2 * (u32)B16(d + tf->loca + 2 * g + 2);
	}
	if (next <= off || next > tf->glyflen)
		return;
	p = d + tf->glyf + off;
	lim = d + tf->glyf + next;
	nc = S16(p);
	if (nc >= 0) {
		u8 *fp, *xp, *yp;
		int ends[MAXCT];

		if (o->nct + nc > MAXCT)
			return;
		for (i = 0; i < nc; i++)
			ends[i] = B16(p + 10 + 2 * i);
		n = nc ? ends[nc - 1] + 1 : 0;
		if (o->npt + n > MAXPT)
			return;
		fp = p + 10 + 2 * nc;
		fp += 2 + B16(fp);		/* past the instructions */
		/* the flags, then the x and the y deltas */
		base = o->npt;
		for (i = 0, xp = fp; i < n && xp < lim; ) {
			fl = *xp++;
			k = (fl & 8) ? *xp++ + 1 : 1;
			while (k-- > 0 && i < n)
				o->on[base + i++] = fl;
		}
		for (v = 0, i = 0; i < n; i++) {
			fl = o->on[base + i];
			if (fl & 2)
				v += (fl & 16) ? *xp++ : -(long)*xp++;
			else if (!(fl & 16))
				v += S16(xp), xp += 2;
			o->x[base + i] = v;
		}
		for (v = 0, i = 0, yp = xp; i < n; i++) {
			fl = o->on[base + i];
			if (fl & 4)
				v += (fl & 32) ? *yp++ : -(long)*yp++;
			else if (!(fl & 32))
				v += S16(yp), yp += 2;
			o->y[base + i] = v;
		}
		if (yp > lim)
			return;
		for (i = 0; i < n; i++) {
			long x = o->x[base + i], y = o->y[base + i];

			o->x[base + i] = ((m[0] * x + m[2] * y) >> 14) + dx;
			o->y[base + i] = ((m[1] * x + m[3] * y) >> 14) + dy;
			o->on[base + i] &= 1;
		}
		for (i = 0; i < nc; i++)
			o->end[o->nct++] = base + ends[i];
		o->npt += n;
		return;
	}
	/* composite: its parts, each moved and perhaps scaled */
	p += 10;
	do {
		long cm[4], a1, a2, ox, oy;

		if (p + 4 > lim)
			return;
		fl = B16(p);
		k = B16(p + 2);
		p += 4;
		if (fl & 1)
			a1 = S16(p), a2 = S16(p + 2), p += 4;
		else
			a1 = (signed char)p[0], a2 = (signed char)p[1], p += 2;
		cm[0] = cm[3] = 1 << 14;
		cm[1] = cm[2] = 0;
		if (fl & 8)
			cm[0] = cm[3] = S16(p), p += 2;
		else if (fl & 0x40)
			cm[0] = S16(p), cm[3] = S16(p + 2), p += 4;
		else if (fl & 0x80)
			cm[0] = S16(p), cm[1] = S16(p + 2), cm[2] = S16(p + 4), cm[3] = S16(p + 6), p += 8;
		if (!(fl & 2))
			a1 = a2 = 0;		/* matched points: not done, rare in these fonts */
		ox = ((m[0] * a1 + m[2] * a2) >> 14) + dx;
		oy = ((m[1] * a1 + m[3] * a2) >> 14) + dy;
		{
			long mm[4];

			mm[0] = (m[0] * cm[0] + m[2] * cm[1]) >> 14;
			mm[1] = (m[1] * cm[0] + m[3] * cm[1]) >> 14;
			mm[2] = (m[0] * cm[2] + m[2] * cm[3]) >> 14;
			mm[3] = (m[1] * cm[2] + m[3] * cm[3]) >> 14;
			glyph(tf, k, o, mm, ox, oy, depth + 1);
		}
	} while (fl & 0x20);
}

/* ---- scan conversion ---- */

struct edge {
	long	x0, y0, x1, y1;		/* 26.6 pixels, y down */
};

static struct edge *edges;
static int nedge, maxedge;

static void
line(x0, y0, x1, y1)
	long x0, y0, x1, y1;
{
	if (nedge == maxedge) {
		maxedge = maxedge ? 2 * maxedge : 512;
		edges = (struct edge *)realloc((char *)edges, maxedge * sizeof *edges);
	}
	edges[nedge].x0 = x0;
	edges[nedge].y0 = y0;
	edges[nedge].x1 = x1;
	edges[nedge].y1 = y1;
	nedge++;
}

static void
curve(x0, y0, cx, cy, x1, y1)
	long x0, y0, cx, cy, x1, y1;
{
	long d = labs(x0 - 2 * cx + x1) + labs(y0 - 2 * cy + y1), px = x0, py = y0, x, y, t, u, nn;
	int n, i;

	/* segments enough that the flattening stays within a quarter pixel */
	for (n = 1; n < 16 && d > 16; n *= 2)
		d /= 4;
	nn = (long)n * n;
	for (i = 1; i <= n; i++) {
		t = i;
		u = n - i;
		x = (x0 * u * u + 2 * cx * u * t + x1 * t * t) / nn;
		y = (y0 * u * u + 2 * cy * u * t + y1 * t * t) / nn;
		line(px, py, x, y);
		px = x;
		py = y;
	}
}

/* an outline (pixels already) to edges: on and off points as TrueType has them */
static void
contours(o)
	struct outline *o;
{
	int c, s, e, i, n;
	long sx, sy, px, py, cx, cy, x, y;
	int havec;

	for (c = 0, s = 0; c < o->nct; s = o->end[c++] + 1) {
		e = o->end[c];
		n = e - s + 1;
		if (n < 2)
			continue;
		/* a start on the curve: the first on point, or between two off ones */
		for (i = 0; i < n && !o->on[s + i]; i++)
			;
		if (i == n) {
			sx = (o->x[s] + o->x[s + 1]) / 2;
			sy = (o->y[s] + o->y[s + 1]) / 2;
			i = 1;
		} else {
			sx = o->x[s + i];
			sy = o->y[s + i];
			i++;
		}
		px = sx;
		py = sy;
		havec = 0;
		{
			int k, j;

			for (k = 0; k < n; k++) {
				j = s + (i - 1 + 1 + k) % n;
				x = o->x[j];
				y = o->y[j];
				if (o->on[j]) {
					if (havec)
						curve(px, py, cx, cy, x, y);
					else
						line(px, py, x, y);
					px = x, py = y, havec = 0;
				} else if (havec) {
					long mx = (cx + x) / 2, my = (cy + y) / 2;

					curve(px, py, cx, cy, mx, my);
					px = mx, py = my, cx = x, cy = y;
				} else
					cx = x, cy = y, havec = 1;
			}
		}
		if (havec)
			curve(px, py, cx, cy, sx, sy);
		else if (px != sx || py != sy)
			line(px, py, sx, sy);
	}
}

struct cross {
	long	x;
	int	dir;
};

static int
bycross(a, b)
	const void *a, *b;
{
	long d = ((struct cross *)a)->x - ((struct cross *)b)->x;

	return d < 0 ? -1 : d > 0;
}

static void
setpix(bits, bpr, w, h, x, y)
	u8 *bits;
	int bpr, w, h, x, y;
{
	if (x >= 0 && x < w && y >= 0 && y < h)
		bits[y * bpr + (x >> 3)] |= 0x80 >> (x & 7);
}

/*
 * Fill by the nonzero rule, sampling pixel centres along rows (vert 0)
 * or columns (vert 1); a span that misses every centre sets the pixel
 * nearest its middle (dropout control).  Pixel (0,0) is at (ox, oy).
 */
static void
scan(bits, bpr, w, h, ox, oy, vert)
	u8 *bits;
	int bpr, w, h, vert;
	long ox, oy;
{
	static struct cross *cr;
	static int maxcr;
	int line_, nl = vert ? w : h, i, nc, wind, a, b;
	long c, lo, hi, x, base = vert ? ox : oy, obase = vert ? oy : ox;

	if (maxcr < nedge) {
		maxcr = nedge + 64;
		cr = (struct cross *)realloc((char *)cr, maxcr * sizeof *cr);
	}
	for (line_ = 0; line_ < nl; line_++) {
		c = base + line_ * 64 + 32;
		for (nc = 0, i = 0; i < nedge; i++) {
			struct edge *e = &edges[i];
			long p0 = vert ? e->x0 : e->y0, p1 = vert ? e->x1 : e->y1;
			long q0 = vert ? e->y0 : e->x0, q1 = vert ? e->y1 : e->x1;

			if (p0 == p1)
				continue;
			if (p0 < p1 ? (c < p0 || c >= p1) : (c < p1 || c >= p0))
				continue;
			cr[nc].x = q0 + (q1 - q0) * (c - p0) / (p1 - p0);
			cr[nc].dir = p1 > p0 ? 1 : -1;
			nc++;
		}
		if (nc < 2)
			continue;
		qsort((char *)cr, nc, sizeof *cr, bycross);
		for (wind = 0, i = 0; i < nc - 1; i++) {
			wind += cr[i].dir;
			if (!wind)
				continue;
			lo = cr[i].x - obase;
			hi = cr[i + 1].x - obase;
			/* centres k*64+32 in [lo, hi) */
			a = (int)((lo - 32 + 63 + 64 * 1024) / 64) - 1024;
			b = (int)((hi - 32 + 63 + 64 * 1024) / 64) - 1024;
			if (a >= b && !vert) {
				x = ((lo + hi) / 2 + 64 * 1024) / 64 - 1024;
				a = (int)x, b = a + 1;
			} else if (a >= b) {
				x = ((lo + hi) / 2 + 64 * 1024) / 64 - 1024;
				setpix(bits, bpr, w, h, line_, (int)x);
				continue;
			} else if (vert)
				continue;	/* columns only fill what rows dropped */
			for (; a < b; a++)
				setpix(bits, bpr, w, h, a, line_);
		}
	}
}

/* ---- sizes ---- */

/* a / b rounded, b > 0 */
static long
rdiv(a, b)
	long a, b;
{
	return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

/* the glyph of character c (0 = FIRST) at this size, rendered now if not yet */
struct ttglyph *
ttf_glyph(f, c)
	struct bfont *f;
	int c;
{
	struct ttsize *ts = f->f_tt;
	struct ttfile *tf = ts->tf;
	struct ttglyph *g = &ts->g[c];
	static struct outline *o;
	long m[4], minx, maxx, x, y, ox, oy;
	int i, gi, w, bpr;

	if (ts->done[c])
		return g;
	ts->done[c] = 1;
	g->bits = 0;
	g->bw = 0;
	g->ox = 0;
	if (!load(tf))
		return g;
	if (!o)
		o = (struct outline *)malloc(sizeof *o);
	o->npt = o->nct = 0;
	gi = gindex(tf, c + FIRST);
	m[0] = m[3] = 1 << 14;
	m[1] = m[2] = 0;
	glyph(tf, gi, o, m, 0L, 0L, 0);
	if (!o->npt)
		return g;
	/* to 26.6 pixels, y down from the cell's top; a synthetic italic leans */
	for (i = 0; i < o->npt; i++) {
		x = o->x[i] + (ts->shear ? o->y[i] * ts->shear / 256 : 0);
		x = rdiv(x * ts->xn, ts->xd);
		y = rdiv(o->y[i] * ts->yn, ts->yd);
		o->x[i] = x;
		o->y[i] = (long)ts->bf.f_ascent * 64 - y;
	}
	minx = maxx = o->x[0];
	for (i = 1; i < o->npt; i++) {
		if (o->x[i] < minx) minx = o->x[i];
		if (o->x[i] > maxx) maxx = o->x[i];
	}
	ox = (minx + 64 * 1024) / 64 - 1024;
	w = (int)((maxx + 63 + 64 * 1024) / 64 - 1024 - ox);
	if (w <= 0)
		w = 1;
	if (w > 1024)
		return g;
	nedge = 0;
	contours(o);
	bpr = (w + 7) / 8;
	g->bits = (u8 *)calloc(1, bpr * ts->bf.f_height + 1);
	g->bw = w;
	g->ox = (short)ox;
	oy = 0;
	scan(g->bits, bpr, w, ts->bf.f_height, ox * 64, oy, 0);
	scan(g->bits, bpr, w, ts->bf.f_height, ox * 64, oy, 1);
	return g;
}

static struct ttfile *
pick(face, bold, italic)
	char *face;
	int bold, italic;
{
	struct ttfile *tf, *best = 0;
	int s, bs = 99;

	for (tf = files; tf; tf = tf->next) {
		if (w16_stricmp(tf->face, face) != 0)
			continue;
		s = (tf->bold != bold) * 2 + (tf->italic != italic);
		if (s < bs)
			bs = s, best = tf;
	}
	return best;
}

/*
 * The face at a height as LOGFONT has it: above 0 the cell, below 0
 * the em (the cell less internal leading), 0 a default.  A width
 * other than 0 stretches it to that average width.
 */
struct bfont *
ttf_font(face, bold, italic, height, width)
	char *face;
	int bold, italic, height, width;
{
	struct ttfile *tf = pick(face, bold, italic);
	struct ttsize *ts;
	struct bfont *f;
	int ppem, shear, c, g, cell;
	long xn, xd;

	if (!tf)
		return 0;
	cell = tf->asc + tf->desc;
	if (height == 0)
		height = -12;
	if (height > 0)
		ppem = (int)(((long)height * tf->upem + cell / 2) / cell);
	else
		ppem = -height;
	if (ppem < 1)
		ppem = 1;
	if (ppem > 400)
		ppem = 400;
	if (width > 400)
		width = 400;
	xn = width > 0 ? width * 64L : ppem * 64L;
	xd = width > 0 ? tf->avgw : tf->upem;
	shear = italic && !tf->italic ? 53 : 0;		/* tan 11.7 degrees, in 1/256 */
	for (ts = sizes; ts; ts = ts->next)
		if (ts->tf == tf && ts->ppem == ppem && ts->xn == xn && ts->xd == xd && ts->shear == shear)
			return &ts->bf;
	if (!load(tf))
		return 0;
	ts = (struct ttsize *)calloc(1, sizeof *ts);
	ts->tf = tf;
	ts->ppem = ppem;
	ts->xn = xn;
	ts->xd = xd;
	ts->yn = ppem * 64L;
	ts->yd = tf->upem;
	ts->shear = shear;
	f = &ts->bf;
	f->f_face = tf->face;
	f->f_points = (ppem * 72 + 48) / 96;
	f->f_ascent = (int)(((long)tf->asc * ppem + tf->upem / 2) / tf->upem);
	f->f_descent = (int)(((long)tf->desc * ppem + tf->upem / 2) / tf->upem);
	f->f_height = f->f_ascent + f->f_descent;
	f->f_leading = f->f_height > ppem ? f->f_height - ppem : 0;
	f->f_extlead = 0;
	f->f_weight = tf->weight;
	f->f_italic = tf->italic || shear;
	f->f_pitch = tf->fixed;
	f->f_family = tf->family;
	f->f_res = 96;
	f->f_charset = tf->symbol ? 2 : 0;
	f->f_first = FIRST;
	f->f_last = 255;
	f->f_default = tf->symbol ? FIRST : 0x80;
	f->f_break = ' ';
	f->f_avgw = (int)(rdiv(tf->avgw * xn, xd) + 32) / 64;
	f->f_maxw = (int)(rdiv(tf->maxw * xn, xd) + 32) / 64;
	for (c = 0; c < NCH; c++) {
		g = gindex(tf, c + FIRST);
		ts->adv[c] = (unsigned short)((rdiv(advance(tf, g) * xn, xd) + 32) / 64);
		ts->w[c] = ts->adv[c] > 255 ? 255 : ts->adv[c];
	}
	f->f_w = ts->w;
	f->f_adv = ts->adv;
	f->f_tt = ts;
	ts->next = sizes;
	sizes = ts;
	return f;
}

/* a TrueType face's name as the font list spells it */
char *
ttf_face(name)
	char *name;
{
	struct ttfile *tf;

	if (!name || !*name)
		return 0;
	for (tf = files; tf; tf = tf->next)
		if (w16_stricmp(tf->face, name) == 0)
			return tf->face;
	return 0;
}

/* a TrueType face for a family (FF_ROMAN, FF_SWISS ...) */
char *
ttf_family(fam, fixed)
	int fam, fixed;
{
	struct ttfile *tf;

	for (tf = files; tf; tf = tf->next)
		if (!tf->symbol && (fixed ? tf->fixed : tf->family == fam))
			return tf->face;
	return 0;
}

/*
 * For EnumFonts: without a face the i-th face (its regular style), with
 * one the i-th style of that face.
 */
struct bfont *
ttf_enum(i, face)
	int i;
	char *face;
{
	struct ttfile *tf, *o;
	int k = 0;

	for (tf = files; tf; tf = tf->next) {
		if (face) {
			if (w16_stricmp(tf->face, face) == 0 && k++ == i)
				return ttf_font(tf->face, tf->bold, tf->italic, -12, 0);
			continue;
		}
		for (o = files; o != tf; o = o->next)
			if (w16_stricmp(o->face, tf->face) == 0)
				break;
		if (o == tf && k++ == i)
			return ttf_font(tf->face, 0, 0, -12, 0);
	}
	return 0;
}
