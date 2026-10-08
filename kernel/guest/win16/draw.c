/*
 * draw.c -- drawing on 8-bit surfaces through a DC's region: fills
 * with brushes and ROPs, lines with pens, blits between surfaces
 * (monochrome ones converting through the text and background colours,
 * as Windows does), polygons, ellipses and text.
 *
 * Coordinates are surface pixels; the callers in gdi.c have mapped
 * them.  ROPs work on palette indexes, as on an 8-bit Windows display.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "win.h"

/* ---- raster operations ---- */

/* rop3 on pattern, source and destination values (bitwise) */
static u32
rop3(rop, p, s, d)
	int rop;
	u32 p, s, d;
{
	u32 r = 0;

	switch (rop) {
	case 0x00: return 0;
	case 0xff: return 0xff;
	case 0xcc: return s;
	case 0xf0: return p;
	case 0xaa: return d;
	case 0x55: return ~d;
	case 0x33: return ~s;
	case 0x66: return s ^ d;
	case 0x88: return s & d;
	case 0xee: return s | d;
	case 0x5a: return p ^ d;
	case 0x44: return s & ~d;
	case 0x11: return ~(s | d);
	case 0xc0: return p & s;
	case 0xbb: return ~s | d;
	case 0xfb: return p | ~s | d;
	case 0xe2: return ((p ^ d) & s) ^ d;
	}
	if (rop & 0x01) r |= ~p & ~s & ~d;
	if (rop & 0x02) r |= ~p & ~s & d;
	if (rop & 0x04) r |= ~p & s & ~d;
	if (rop & 0x08) r |= ~p & s & d;
	if (rop & 0x10) r |= p & ~s & ~d;
	if (rop & 0x20) r |= p & ~s & d;
	if (rop & 0x40) r |= p & s & ~d;
	if (rop & 0x80) r |= p & s & d;
	return r;
}

/* R2_* on pen and destination */
static u32
rop2(r2, p, d)
	int r2;
	u32 p, d;
{
	switch (r2) {
	case R2_BLACK: return 0;
	case R2_NOTMERGEPEN: return ~(p | d);
	case R2_MASKNOTPEN: return ~p & d;
	case R2_NOTCOPYPEN: return ~p;
	case R2_MASKPENNOT: return p & ~d;
	case R2_NOT: return ~d;
	case R2_XORPEN: return p ^ d;
	case R2_NOTMASKPEN: return ~(p & d);
	case R2_MASKPEN: return p & d;
	case R2_NOTXORPEN: return ~(p ^ d);
	case R2_NOP: return d;
	case R2_MERGENOTPEN: return ~p | d;
	case R2_COPYPEN: return p;
	case R2_MERGEPENNOT: return p | ~d;
	case R2_MERGEPEN: return p | d;
	case R2_WHITE: return 0xff;
	}
	return p;
}

/* a pixel value fit for the surface: mono ones hold 0 and 1 */
static u32
fit(s, v)
	struct surf *s;
	u32 v;
{
	return s->mono ? v & 1 : v & 0xff;
}

/* a palette index for the surface (mono: the nearest of black and white) */
static int
colorfor(dc, c)
	struct dc *dc;
	COLORREF c;
{
	if (dc->s->mono)
		return pal_mono(c);
	return pal_index(c);
}

/* ---- brushes ---- */

/* the brush's pattern for this DC as 64 pixel values */
static void
brushpat(dc, hb, pat)
	struct dc *dc;
	u32 hb;
	u8 *pat;
{
	struct gobj *o = gobj(hb, OBJ_BRUSH);
	struct brush *b;
	int i, fg, bg;

	if (!o) {
		memset(pat, colorfor(dc, RGB(255, 255, 255)), 64);
		return;
	}
	b = &o->u.brush;
	if (b->style == BS_SOLID) {
		memset(pat, colorfor(dc, b->color), 64);
		return;
	}
	if (b->monopat) {
		fg = colorfor(dc, b->style == BS_HATCHED ? b->color : dc->st.text);
		bg = colorfor(dc, dc->st.bk);
		for (i = 0; i < 64; i++)
			pat[i] = b->pat[i] ? bg : fg;
		return;
	}
	for (i = 0; i < 64; i++)
		pat[i] = dc->s->mono ? (syspal[b->pat[i]][0] + syspal[b->pat[i]][1] + syspal[b->pat[i]][2] >= 384) :
		    b->pat[i];
}

int
brush_index(hb)
	u32 hb;
{
	struct gobj *o = gobj(hb, OBJ_BRUSH);

	if (!o || o->u.brush.style != BS_SOLID)
		return -1;
	return pal_index(o->u.brush.color);
}

/* ---- fills ---- */

void
d_fillcolor(dc, r, idx)
	struct dc *dc;
	struct rect *r;
	int idx;
{
	struct rgn *g = dc_clip(dc);
	struct surf *s = dc->s;
	struct rect m;
	int i, y;

	idx = fit(s, idx);
	for (i = 0; i < g->n; i++) {
		if (!r_and(&m, &g->r[i], r))
			continue;
		for (y = m.t; y < m.b; y++)
			memset(s->pix + y * s->rowb + m.l, idx, m.r - m.l);
		if (s == &screen)
			scr_dirty(&m);
	}
}

/* the ROP3 that fills with the brush as the DC's ROP2 mixes the pen: shapes' insides */
int
d_fillrop(dc)
	struct dc *dc;
{
	int r = (dc->st.rop2 - 1) & 15, code = 0, k, p, d;

	for (k = 0; k < 8; k++) {
		p = k >> 2 & 1;
		d = k & 1;
		if (r >> (p * 2 + d) & 1)
			code |= 1 << k;
	}
	return code;
}

/* rop: the ROP3 code (high byte of the low word of a raster op) on pattern and destination */
void
d_fill(dc, r, hb, rop)
	struct dc *dc;
	struct rect *r;
	u32 hb;
	int rop;
{
	struct rgn *g = dc_clip(dc);
	struct surf *s = dc->s;
	struct rect m;
	u8 pat[64], *p;
	int i, x, y, bx, by, solid = 1;
	struct gobj *o = gobj(hb, OBJ_BRUSH);

	if (o && o->u.brush.style == BS_NULL)	/* a hollow brush fills nothing, whatever the mix */
		return;
	brushpat(dc, hb, pat);
	for (i = 1; i < 64; i++)
		if (pat[i] != pat[0])
			solid = 0;
	if (solid && rop == 0xf0) {
		d_fillcolor(dc, r, pat[0]);
		return;
	}
	bx = dc->st.bx + dc->ox;
	by = dc->st.by + dc->oy;
	for (i = 0; i < g->n; i++) {
		if (!r_and(&m, &g->r[i], r))
			continue;
		for (y = m.t; y < m.b; y++) {
			p = s->pix + y * s->rowb;
			for (x = m.l; x < m.r; x++)
				p[x] = fit(s, rop3(rop, pat[((y - by) & 7) * 8 + ((x - bx) & 7)], 0, p[x]));
		}
		if (s == &screen)
			scr_dirty(&m);
	}
}

void
d_invert(dc, r)
	struct dc *dc;
	struct rect *r;
{
	d_fill(dc, r, 0, 0x55);
}

void
d_frame(dc, r, idx)
	struct dc *dc;
	struct rect *r;
	int idx;
{
	struct rect e;

	r_set(&e, r->l, r->t, r->r, r->t + 1);
	d_fillcolor(dc, &e, idx);
	r_set(&e, r->l, r->b - 1, r->r, r->b);
	d_fillcolor(dc, &e, idx);
	r_set(&e, r->l, r->t, r->l + 1, r->b);
	d_fillcolor(dc, &e, idx);
	r_set(&e, r->r - 1, r->t, r->r, r->b);
	d_fillcolor(dc, &e, idx);
}

void
d_pixel(dc, x, y, idx)
	struct dc *dc;
	int x, y, idx;
{
	struct rgn *g = dc_clip(dc);
	struct rect m;
	u8 *p;

	if (x < g->box.l || x >= g->box.r || y < g->box.t || y >= g->box.b || !rgn_ptin(g, x, y))
		return;
	p = dc->s->pix + y * dc->s->rowb + x;
	*p = fit(dc->s, rop2(dc->st.rop2, idx, *p));
	if (dc->s == &screen) {
		r_set(&m, x, y, x + 1, y + 1);
		scr_dirty(&m);
	}
}

/* ---- lines ---- */

static int dashes[5][8] = {
	{ 0 }, { 18, 6, 0 }, { 3, 3, 0 }, { 9, 6, 3, 6, 0 }, { 9, 3, 3, 3, 3, 3, 0 }
};

/* the pen's pixels from (x0,y0) to (x1,y1), the last one left out */
void
d_line(dc, x0, y0, x1, y1)
	struct dc *dc;
	int x0, y0, x1, y1;
{
	struct gobj *o = gobj(dc->st.pen, OBJ_PEN);
	struct rgn *g = dc_clip(dc);
	struct surf *s = dc->s;
	struct rect m, d;
	int dx, dy, sx, sy, err, e2, idx, w, style, dn = 0, dl, on = 1, i, k, bgidx;
	u8 *p;

	if (!o || o->u.pen.style == PS_NULL)
		return;
	idx = colorfor(dc, o->u.pen.color);
	w = o->u.pen.width > 1 ? o->u.pen.width : 1;
	style = o->u.pen.style;
	if (style > PS_DASHDOTDOT || w > 1)
		style = PS_SOLID;
	bgidx = colorfor(dc, dc->st.bk);
	dl = dashes[style][0];
	/* horizontal and vertical solid lines: spans */
	if (style == PS_SOLID && dc->st.rop2 == R2_COPYPEN && (y0 == y1 || x0 == x1)) {
		if (y0 == y1)
			r_set(&m, x0 < x1 ? x0 : x1 + 1, y0 - w / 2, x0 < x1 ? x1 : x0 + 1, y0 - w / 2 + w);
		else
			r_set(&m, x0 - w / 2, y0 < y1 ? y0 : y1 + 1, x0 - w / 2 + w, y0 < y1 ? y1 : y0 + 1);
		d_fillcolor(dc, &m, idx);
		return;
	}
	dx = abs(x1 - x0);
	dy = -abs(y1 - y0);
	sx = x0 < x1 ? 1 : -1;
	sy = y0 < y1 ? 1 : -1;
	err = dx + dy;
	while (x0 != x1 || y0 != y1) {
		if (style != PS_SOLID) {
			if (dl-- <= 0) {
				dn++;
				if (!dashes[style][dn])
					dn = 0;
				dl = dashes[style][dn] - 1;
				on = !(dn & 1);
			}
		}
		if (on || dc->st.bkmode == OPAQUE) {
			for (i = 0; i < w; i++)
				for (k = 0; k < w; k++) {
					int px = x0 + i - w / 2, py = y0 + k - w / 2;

					if (px < g->box.l || px >= g->box.r || py < g->box.t || py >= g->box.b ||
					    !rgn_ptin(g, px, py))
						continue;
					p = s->pix + py * s->rowb + px;
					*p = fit(s, rop2(dc->st.rop2, on ? idx : bgidx, *p));
				}
		}
		e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
	if (s == &screen) {
		r_set(&d, g->box.l, g->box.t, g->box.r, g->box.b);
		scr_dirty(&d);
	}
}

/* ---- blits ---- */

/*
 * dst <- rop(pattern, src, dst) over w x h.  A mono source on a colour
 * destination: 0 is the text colour, 1 the background; a colour source
 * on a mono destination: the background colour is 1, the rest 0.
 */
void
d_blt(dc, dx, dy, w, h, sdc, sx, sy, rop)
	struct dc *dc, *sdc;
	int dx, dy, w, h, sx, sy;
	u32 rop;
{
	struct rgn *g = dc_clip(dc);
	struct surf *s = dc->s, *ss = sdc ? sdc->s : 0;
	struct rect r, m;
	u8 pat[64], *dp, *sp, *tmp = 0, *srcrow;
	int i, x, y, code = rop >> 16 & 0xff, usesrc, usepat, fg = 0, bg = 0, bx, by;
	int srcbg = 0;
	u32 sv, pv;

	usesrc = ((code >> 2) ^ code) & 0x33;
	usepat = ((code >> 4) ^ code) & 0x0f;
	if (!usesrc) {
		r_set(&r, dx, dy, dx + w, dy + h);
		d_fill(dc, &r, dc->st.brush, code);
		return;
	}
	if (!ss)
		return;
	if (usepat)
		brushpat(dc, dc->st.brush, pat);
	if (ss->mono && !s->mono) {
		fg = pal_index(dc->st.text);
		bg = pal_index(dc->st.bk);
	}
	if (!ss->mono && s->mono)
		srcbg = pal_index(sdc->st.bk);
	/* the source clipped to its surface: outside it the source reads as 0 */
	r_set(&r, dx, dy, dx + w, dy + h);
	/* overlapping blits within one surface go through a copy */
	if (ss == s) {
		tmp = (u8 *)malloc(w * h + 1);
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				int px = sx + x, py = sy + y;

				tmp[y * w + x] = px >= 0 && py >= 0 && px < ss->w && py < ss->h ?
				    ss->pix[py * ss->rowb + px] : 0;
			}
	}
	bx = dc->st.bx + dc->ox;
	by = dc->st.by + dc->oy;
	for (i = 0; i < g->n; i++) {
		if (!r_and(&m, &g->r[i], &r))
			continue;
		for (y = m.t; y < m.b; y++) {
			int py = sy + (y - dy);

			dp = s->pix + y * s->rowb;
			srcrow = tmp ? tmp + (y - dy) * w : (py >= 0 && py < ss->h ? ss->pix + py * ss->rowb : 0);
			for (x = m.l; x < m.r; x++) {
				int px = sx + (x - dx);

				if (tmp)
					sv = srcrow[x - dx];
				else
					sv = srcrow && px >= 0 && px < ss->w ? srcrow[px] : 0;
				if (ss->mono && !s->mono)
					sv = sv ? bg : fg;
				else if (!ss->mono && s->mono)
					sv = sv == srcbg ? 1 : 0;
				else if (ss->mono && s->mono)
					sv &= 1;
				pv = usepat ? pat[((y - by) & 7) * 8 + ((x - bx) & 7)] : 0;
				if (code == 0xcc)
					dp[x] = sv;
				else
					dp[x] = fit(s, rop3(code, pv, sv, dp[x]));
			}
		}
		if (s == &screen)
			scr_dirty(&m);
	}
	if (tmp)
		free(tmp);
}

void
d_stretch(dc, dx, dy, dw, dh, sdc, sx, sy, sw, sh, rop)
	struct dc *dc, *sdc;
	int dx, dy, dw, dh, sx, sy, sw, sh;
	u32 rop;
{
	struct surf t, *ss = sdc->s, *keep;
	int x, y, fx, fy, ox, oy, okeep;
	struct dc tmp;

	if (!dw || !dh || !sw || !sh)
		return;
	if (dw == sw && dh == sh && dw > 0 && dh > 0) {
		d_blt(dc, dx, dy, dw, dh, sdc, sx, sy, rop);
		return;
	}
	/* scale the source into a scratch surface, then blit that */
	t.w = abs(dw);
	t.h = abs(dh);
	t.rowb = t.w;
	t.mono = ss->mono;
	t.pix = (u8 *)malloc(t.w * t.h + 1);
	for (y = 0; y < t.h; y++) {
		fy = sy + (int)((long long)(dh < 0 ? t.h - 1 - y : y) * sh / t.h);
		if (sh < 0)
			fy = sy + sh + 1 + (int)((long long)(dh < 0 ? t.h - 1 - y : y) * (-sh) / t.h);
		for (x = 0; x < t.w; x++) {
			fx = sx + (int)((long long)(dw < 0 ? t.w - 1 - x : x) * sw / t.w);
			if (sw < 0)
				fx = sx + sw + 1 + (int)((long long)(dw < 0 ? t.w - 1 - x : x) * (-sw) / t.w);
			t.pix[y * t.w + x] = fx >= 0 && fy >= 0 && fx < ss->w && fy < ss->h ?
			    ss->pix[fy * ss->rowb + fx] : 0;
		}
	}
	tmp = *sdc;
	keep = tmp.s;
	okeep = 0;
	(void)keep;
	(void)okeep;
	tmp.s = &t;
	ox = dw < 0 ? dx + dw + 1 : dx;
	oy = dh < 0 ? dy + dh + 1 : dy;
	d_blt(dc, ox, oy, t.w, t.h, &tmp, 0, 0, rop);
	free(t.pix);
}

/* ---- polygons and ellipses ---- */

struct pt {
	int x, y;
};

static int
icmp(a, b)
	const void *a, *b;
{
	return *(const int *)a - *(const int *)b;
}

/* fill a polygon (alternate or winding) with the brush */
static void
polyfill(dc, p, n, winding)
	struct dc *dc;
	struct pt *p;
	int n, winding;
{
	int miny = p[0].y, maxy = p[0].y, y, i, j, nx, *xs, *dirs;
	struct rect r;

	for (i = 1; i < n; i++) {
		if (p[i].y < miny) miny = p[i].y;
		if (p[i].y > maxy) maxy = p[i].y;
	}
	xs = (int *)malloc(sizeof(int) * (n + 1) * 2);
	dirs = xs + n + 1;
	for (y = miny; y < maxy; y++) {
		double fy = y + 0.5;

		nx = 0;
		for (i = 0; i < n; i++) {
			struct pt *a = &p[i], *b = &p[(i + 1) % n];

			if (a->y == b->y)
				continue;
			if ((fy < a->y && fy < b->y) || (fy >= a->y && fy >= b->y))
				continue;
			xs[nx] = (int)(a->x + (fy - a->y) * (b->x - a->x) / (double)(b->y - a->y) + 0.5);
			dirs[nx] = b->y > a->y ? 1 : -1;
			nx++;
		}
		if (!winding) {
			qsort(xs, nx, sizeof *xs, icmp);
			for (j = 0; j + 1 < nx; j += 2) {
				r_set(&r, xs[j], y, xs[j + 1], y + 1);
				d_fill(dc, &r, dc->st.brush, d_fillrop(dc));
			}
		} else {
			/* sort with directions */
			int k, t, w = 0, start = 0;

			for (j = 1; j < nx; j++)
				for (k = j; k > 0 && xs[k - 1] > xs[k]; k--) {
					t = xs[k]; xs[k] = xs[k - 1]; xs[k - 1] = t;
					t = dirs[k]; dirs[k] = dirs[k - 1]; dirs[k - 1] = t;
				}
			for (j = 0; j < nx; j++) {
				if (w == 0)
					start = xs[j];
				w += dirs[j];
				if (w == 0) {
					r_set(&r, start, y, xs[j], y + 1);
					d_fill(dc, &r, dc->st.brush, d_fillrop(dc));
				}
			}
		}
	}
	free(xs);
}

void
d_poly(dc, pts, n, fill, outline)
	struct dc *dc;
	int *pts;		/* x, y pairs, device */
	int n, fill, outline;
{
	struct pt *p = (struct pt *)pts;
	int i;

	if (n < 2)
		return;
	if (fill && n > 2)
		polyfill(dc, p, n, dc->st.polyfill == 2);
	if (outline) {
		for (i = 0; i + 1 < n; i++)
			d_line(dc, p[i].x, p[i].y, p[i + 1].x, p[i + 1].y);
		if (fill)
			d_line(dc, p[n - 1].x, p[n - 1].y, p[0].x, p[0].y);
	}
}

/* the half-width of an ellipse at row y of its box */
static int
halfw(r, y)
	struct rect *r;
	int y;
{
	double a = (r->r - r->l) / 2.0, b = (r->b - r->t) / 2.0, cy = (r->t + r->b) / 2.0;
	double t = (y + 0.5 - cy) / b, v;

	v = 1 - t * t;
	if (v <= 0)
		return -1;
	return (int)(a * sqrt(v) + 0.5);
}

void
d_ellipse(dc, r, fill, outline)
	struct dc *dc;
	struct rect *r;
	int fill, outline;
{
	struct gobj *o = gobj(dc->st.pen, OBJ_PEN);
	int y, h, idx, cx2 = r->l + r->r, l, lp, ln, e;
	struct rect s;

	if (R_EMPTY(r))
		return;
	idx = o ? colorfor(dc, o->u.pen.color) : 0;
	if (o && o->u.pen.style == PS_NULL)
		outline = 0;
	for (y = r->t; y < r->b; y++) {
		if ((h = halfw(r, y)) < 0)
			continue;
		l = (cx2 + 1) / 2 - h;
		if (fill) {
			r_set(&s, l, y, cx2 - l, y + 1);
			d_fill(dc, &s, dc->st.brush, d_fillrop(dc));
		}
		if (!outline)
			continue;
		/* the edge on this row reaches to where the rows above and below start */
		lp = y > r->t && halfw(r, y - 1) >= 0 ? (cx2 + 1) / 2 - halfw(r, y - 1) : cx2 / 2;
		ln = y + 1 < r->b && halfw(r, y + 1) >= 0 ? (cx2 + 1) / 2 - halfw(r, y + 1) : cx2 / 2;
		e = l + 1;
		if (lp > e)
			e = lp;
		if (ln > e)
			e = ln;
		if (e > (cx2 + 1) / 2)
			e = (cx2 + 1) / 2;
		if (e <= l)
			e = l + 1;
		r_set(&s, l, y, e, y + 1);
		d_fillcolor(dc, &s, idx);
		r_set(&s, cx2 - e, y, cx2 - l, y + 1);
		d_fillcolor(dc, &s, idx);
	}
}

/* ---- text ---- */

int
text_width(f, s, n)
	struct bfont *f;
	char *s;
	int n;
{
	int w = 0, c;

	while (n-- > 0) {
		c = (u8)*s++;
		if (c < f->f_first || c > f->f_last)
			c = f->f_first == 0x20 ? '?' : f->f_first;
		w += f->f_adv ? f->f_adv[c - f->f_first] : f->f_w[c - f->f_first];
	}
	return w;
}

/*
 * Draw n characters with the cell's top left at (x, y), in the text
 * colour; dx[] gives each character's advance when not 0.  Bold adds
 * one pixel to each.
 */
void
glyphs(dc, f, x, y, s, n, dx, bold, underline, strike)
	struct dc *dc;
	struct bfont *f;
	int x, y, n, *dx, bold, underline, strike;
	char *s;
{
	struct rgn *g = dc_clip(dc);
	struct surf *sf = dc->s;
	int c, i, gx, gy, w, bw, ox, bpr, fg = colorfor(dc, dc->st.text), k, x0 = x;
	u8 *bits, *p;
	struct rect cell, m, d;

	fg = fit(sf, fg);
	for (k = 0; k < n; k++) {
		c = (u8)s[k];
		if (c < f->f_first || c > f->f_last)
			c = '?';
		c -= f->f_first;
		if (f->f_tt) {
			struct ttglyph *t = ttf_glyph(f, c);

			w = f->f_adv[c];
			bits = t->bits;
			bw = t->bw;
			ox = t->ox;
		} else {
			w = bw = f->f_w[c];
			bits = f->f_bits + f->f_off[c];
			ox = 0;
		}
		bpr = (bw + 7) / 8;
		r_set(&cell, x + ox, y, x + ox + bw + bold, y + f->f_height);
		if (bits && rgn_rectin(g, &cell))
			for (i = 0; i < g->n; i++) {
				if (!r_and(&m, &g->r[i], &cell))
					continue;
				for (gy = m.t; gy < m.b; gy++) {
					u8 *row = bits + (gy - y) * bpr;

					p = sf->pix + gy * sf->rowb;
					for (gx = m.l; gx < m.r; gx++) {
						int bx = gx - x - ox;

						if ((bx < bw && (row[bx >> 3] & (0x80 >> (bx & 7)))) ||
						    (bold && bx > 0 && bx - 1 < bw && (row[(bx - 1) >> 3] & (0x80 >> ((bx - 1) & 7)))))
							p[gx] = fg;
					}
				}
			}
		x += dx ? dx[k] : w;	/* a fake bold strikes one pixel over, the advance as it is */
	}
	if (underline || strike) {
		r_set(&m, x0, y + (underline ? f->f_ascent + 1 : f->f_ascent * 2 / 3), x,
		    y + (underline ? f->f_ascent + 2 : f->f_ascent * 2 / 3 + 1));
		if (m.t >= y + f->f_height)
			m.t = y + f->f_height - 1, m.b = m.t + 1;
		d_fillcolor(dc, &m, fg);
	}
	if (sf == &screen) {
		/* TrueType glyphs may reach a little past their cells */
		r_set(&d, x0 - (f->f_tt ? f->f_height / 2 : 0), y, x + 1 + (f->f_tt ? f->f_height / 2 : 0), y + f->f_height);
		r_and(&d, &d, &g->box);
		scr_dirty(&d);
	}
}
