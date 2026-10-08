/*
 * ttf.c -- TrueType, which Windows 3.1's GDI renders itself: the .TTF
 * files the user's Windows brings (Arial, Times New Roman, Courier New,
 * Symbol, Wingdings), named in WIN.INI's [fonts] through their .FOT
 * headers.  Each face at each size becomes a bitmap font like the .FON
 * ones, its advances computed at once (the font's hdmx where it has the
 * size, as GDI took them, else the hinted advances) and its glyphs made
 * when first drawn.  The glyphs are FreeType's (built with ft/w16ftopt.h):
 * each outline hinted by the font's own programs with the classic (v35)
 * interpreter, as Windows 3.1's rasterizer ran them, and scan converted
 * in black and white.  A synthetic italic leans after hinting.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "win.h"
#include "font.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

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
	u32	hdmx, hdmxlen;	/* the hinted advances at some sizes, 0 none */
	FT_Face	ftface;		/* FreeType's, over d (kept while it lives) */
};

struct ttsize {
	struct ttsize *next;
	struct ttfile *tf;
	int	ppem, shear;
	long	xn, xd, yn, yd;		/* font units to 26.6 pixels: * n / d */
	FT_Size	size;			/* FreeType's for this size: its hinting state */
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
	if ((t = table(d, tf->n, "hdmx", &tf->hdmxlen)) != 0)
		tf->hdmx = t - d;
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

/* the hdmx record for ppem, as GDI takes its advances from: 0 none */
static u8 *
hdmx(tf, ppem)
	struct ttfile *tf;
	int ppem;
{
	u8 *h = tf->d + tf->hdmx, *r;
	long i, n, size;

	if (!tf->hdmx || tf->hdmxlen < 8)
		return 0;
	n = S16(h + 2);
	size = (long)B32(h + 4);
	if (size < 2 + tf->nglyph || 8 + n * size > (long)tf->hdmxlen)
		return 0;
	for (i = 0, r = h + 8; i < n; i++, r += size)
		if (r[0] == ppem)
			return r;
	return 0;
}

/* ---- sizes ---- */

/* a / b rounded, b > 0 */
static long
rdiv(a, b)
	long a, b;
{
	return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

/* FreeType, once */
static FT_Library ftlib;

/* the face's FreeType face and this size's FreeType size, made when first needed: 0 if they cannot be */
static int
ftsize(ts)
	struct ttsize *ts;
{
	struct ttfile *tf = ts->tf;
	FT_Size_RequestRec rq;

	if (ts->size)
		return FT_Activate_Size(ts->size) == 0;
	if (!ftlib && FT_Init_FreeType(&ftlib) != 0) {
		ftlib = 0;
		return 0;
	}
	if (!tf->ftface && (!load(tf) || FT_New_Memory_Face(ftlib, tf->d, tf->n, 0, &tf->ftface) != 0)) {
		tf->ftface = 0;
		return 0;
	}
	if (FT_New_Size(tf->ftface, &ts->size) != 0) {
		ts->size = 0;
		return 0;
	}
	FT_Activate_Size(ts->size);
	/* the em in 26.6 pixels: ppem high, across as the size stretches it */
	memset((char *)&rq, 0, sizeof rq);
	rq.type = FT_SIZE_REQUEST_TYPE_NOMINAL;
	rq.height = ts->yn;
	rq.width = rdiv(tf->upem * ts->xn, ts->xd);
	if (FT_Request_Size(tf->ftface, &rq) != 0) {
		FT_Done_Size(ts->size);
		ts->size = 0;
		return 0;
	}
	return 1;
}

/* glyph gi of the size hinted (and leant) in FreeType's slot: 0 if it cannot be */
static FT_GlyphSlot
ftload(ts, gi)
	struct ttsize *ts;
	int gi;
{
	FT_Face face;
	FT_Matrix m;

	if (!ftsize(ts))
		return 0;
	face = ts->tf->ftface;
	if (FT_Load_Glyph(face, (FT_UInt)gi, FT_LOAD_TARGET_MONO) != 0)
		return 0;
	if (ts->shear && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
		/* the synthetic italic, after hinting: x += y * shear / 256 */
		m.xx = 0x10000L;
		m.xy = (FT_Fixed)ts->shear << 8;
		m.yx = 0;
		m.yy = 0x10000L;
		FT_Outline_Transform(&face->glyph->outline, &m);
	}
	return face->glyph;
}

/* the glyph of character c (0 = FIRST) at this size, rendered now if not yet */
struct ttglyph *
ttf_glyph(f, c)
	struct bfont *f;
	int c;
{
	struct ttsize *ts = f->f_tt;
	struct ttglyph *g = &ts->g[c];
	FT_GlyphSlot slot;
	FT_Bitmap *b;
	int bpr, r, y, x;

	if (ts->done[c])
		return g;
	ts->done[c] = 1;
	g->bits = 0;
	g->bw = 0;
	g->ox = 0;
	if (!load(ts->tf) || (slot = ftload(ts, gindex(ts->tf, c + FIRST))) == 0 ||
	    FT_Render_Glyph(slot, FT_RENDER_MODE_MONO) != 0)
		return g;
	b = &slot->bitmap;
	if (b->width <= 0 || b->rows <= 0 || b->width > 1024 || b->pixel_mode != FT_PIXEL_MODE_MONO)
		return g;
	/* into the cell: f_height rows from its top, the baseline f_ascent down */
	bpr = (b->width + 7) / 8;
	g->bits = (u8 *)calloc(1, bpr * ts->bf.f_height + 1);
	g->bw = b->width;
	g->ox = slot->bitmap_left;
	for (r = 0; r < (int)b->rows; r++) {
		y = ts->bf.f_ascent - slot->bitmap_top + r;
		if (y < 0 || y >= ts->bf.f_height)
			continue;
		for (x = 0; x < bpr; x++)
			g->bits[y * bpr + x] = b->buffer[r * b->pitch + x];
	}
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
	return ttf_fontx(face, bold, italic, height, width, 1, 1);
}

/* ttf_font on a device whose pixels are not square: xdpi/ydpi as wide */
struct bfont *
ttf_fontx(face, bold, italic, height, width, xdpi, ydpi)
	char *face;
	int bold, italic, height, width, xdpi, ydpi;
{
	struct ttfile *tf = pick(face, bold, italic);
	struct ttsize *ts;
	struct bfont *f;
	int ppem, shear, c, g, cell;
	u8 *hd;
	long xn, xd;
	int stretched = 0;

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
	if (xdpi > 0 && ydpi > 0 && xdpi != ydpi && width <= 0) {
		int a = xdpi, b = ydpi, t;

		while (b) {		/* the ratio at its smallest: 120:144 is 5:6 */
			t = a % b;
			a = b;
			b = t;
		}
		xn *= xdpi / a;
		xd *= ydpi / a;
		stretched = 1;
	}
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
	f->f_points = (ppem * 72 + w16_dpi / 2) / w16_dpi;	/* on the screen */
	f->f_ascent = (int)(((long)tf->asc * ppem + tf->upem / 2) / tf->upem);
	f->f_descent = (int)(((long)tf->desc * ppem + tf->upem / 2) / tf->upem);
	f->f_height = f->f_ascent + f->f_descent;
	f->f_leading = f->f_height > ppem ? f->f_height - ppem : 0;
	f->f_extlead = 0;
	f->f_weight = tf->weight;
	f->f_italic = tf->italic || shear;
	f->f_pitch = tf->fixed;
	f->f_family = tf->family;
	f->f_res = w16_dpi;
	f->f_charset = tf->symbol ? 2 : 0;
	f->f_first = FIRST;
	f->f_last = 255;
	f->f_default = tf->symbol ? FIRST : 0x80;
	f->f_break = ' ';
	f->f_avgw = (int)(rdiv(tf->avgw * xn, xd) + 32) / 64;
	f->f_maxw = (int)(rdiv(tf->maxw * xn, xd) + 32) / 64;
	hd = width > 0 || stretched ? 0 : hdmx(tf, ppem);
	for (c = 0; c < NCH; c++) {
		FT_GlyphSlot slot;

		g = gindex(tf, c + FIRST);
		if (hd && g < tf->nglyph)
			ts->adv[c] = hd[2 + g];
		else if ((slot = ftload(ts, g)) != 0)
			ts->adv[c] = (unsigned short)((slot->advance.x + 32) >> 6);	/* hinted */
		else
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
