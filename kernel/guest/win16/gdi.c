/*
 * gdi.c -- our GDI: objects (pens, brushes, fonts, bitmaps, regions,
 * palettes) and DCs, the mapping modes, and the GDI exports.  Drawing
 * itself is draw.c's; it works in surface pixels, so every call here
 * maps logical coordinates first.
 *
 * The display is 8 bits a pixel with a fixed system palette: Windows'
 * twenty static colours, a 6x6x6 colour cube and a grey ramp.  Solid
 * colours go to the nearest entry (no dithering).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "win.h"

#define	NGOBJ	8192
#define	HBASE	0x0100

static struct gobj *objs[NGOBJ];
u16 stockobj[NSTOCK];
u8 syspal[256][3];
static u8 cache[32768];
static u8 cacheok[32768 / 8];

#define	STR(p)		(gptr(p) ? gptr(p) : "")

int bfonts_count(), pal4(), d_fillrop();
u16 stockbitmap();

/* ---- palette ---- */

static u8 statics[20][3] = {
	{ 0, 0, 0 }, { 0x80, 0, 0 }, { 0, 0x80, 0 }, { 0x80, 0x80, 0 }, { 0, 0, 0x80 },
	{ 0x80, 0, 0x80 }, { 0, 0x80, 0x80 }, { 0xc0, 0xc0, 0xc0 }, { 0xc0, 0xdc, 0xc0 }, { 0xa6, 0xca, 0xf0 },
	{ 0xff, 0xfb, 0xf0 }, { 0xa0, 0xa0, 0xa4 }, { 0x80, 0x80, 0x80 }, { 0xff, 0, 0 }, { 0, 0xff, 0 },
	{ 0xff, 0xff, 0 }, { 0, 0, 0xff }, { 0xff, 0, 0xff }, { 0, 0xff, 0xff }, { 0xff, 0xff, 0xff }
};

void
pal_init()
{
	int i, r, g, b, n = 10;

	for (i = 0; i < 10; i++)
		memcpy(syspal[i], statics[i], 3);
	for (i = 0; i < 10; i++)
		memcpy(syspal[246 + i], statics[10 + i], 3);
	for (r = 0; r < 6; r++)
		for (g = 0; g < 6; g++)
			for (b = 0; b < 6; b++) {
				syspal[n][0] = r * 51;
				syspal[n][1] = g * 51;
				syspal[n][2] = b * 51;
				n++;
			}
	for (i = 0; n < 246; i++, n++)
		syspal[n][0] = syspal[n][1] = syspal[n][2] = 8 + i * 12;
}

static int
nearest(r, g, b)
	int r, g, b;
{
	int k, i, best = 0, bd = 1 << 30, d;

	/*
	 * The 20 static colours first: a colour they have is theirs (white is
	 * 255, black 0, as raster operations on indexes need), not the cube's.
	 */
	for (k = 0; k < 256; k++) {
		i = k < 10 ? k : k < 20 ? 236 + k : k - 10;
		d = (syspal[i][0] - r) * (syspal[i][0] - r) * 3 + (syspal[i][1] - g) * (syspal[i][1] - g) * 4 +
		    (syspal[i][2] - b) * (syspal[i][2] - b) * 2;
		if (d < bd) {
			bd = d;
			best = i;
			if (d == 0)
				break;
		}
	}
	return best;
}

int
pal_index(c)
	COLORREF c;
{
	int k;

	if ((c >> 24) == 1)		/* PALETTEINDEX */
		return c & 0xff;
	k = (CR_R(c) >> 3) << 10 | (CR_G(c) >> 3) << 5 | CR_B(c) >> 3;
	/* exact colours (the statics) are not cached, the rest per 15-bit colour */
	if (!(cacheok[k >> 3] & 1 << (k & 7))) {
		cache[k] = nearest(CR_R(c), CR_G(c), CR_B(c));
		cacheok[k >> 3] |= 1 << (k & 7);
	}
	if (syspal[cache[k]][0] == CR_R(c) && syspal[cache[k]][1] == CR_G(c) && syspal[cache[k]][2] == CR_B(c))
		return cache[k];
	return nearest(CR_R(c), CR_G(c), CR_B(c));
}

int
pal_mono(c)
	COLORREF c;
{
	if ((c >> 24) == 1) {
		int i = c & 0xff;

		return syspal[i][0] + syspal[i][1] + syspal[i][2] >= 384;
	}
	return CR_R(c) + CR_G(c) + CR_B(c) >= 384;
}

static COLORREF
idxcolor(i)
	int i;
{
	return RGB(syspal[i & 0xff][0], syspal[i & 0xff][1], syspal[i & 0xff][2]);
}

/* ---- objects ---- */

struct gobj *
gobj(h, type)
	u32 h;
	int type;
{
	int i;

	h &= 0xffff;
	if (h < HBASE || (h - HBASE) & 3)
		return 0;
	i = (h - HBASE) >> 2;
	if (i >= NGOBJ || !objs[i])
		return 0;
	if (type && objs[i]->type != type &&
	    !(type == OBJ_DC && (objs[i]->type == OBJ_MEMDC || objs[i]->type == OBJ_METADC)))
		return 0;
	return objs[i];
}

u16
gobj_new(type)
	int type;
{
	static int hint;
	int i, k;

	for (k = 0; k < NGOBJ; k++) {
		i = (hint + k) % NGOBJ;
		if (!objs[i]) {
			objs[i] = (struct gobj *)calloc(1, sizeof *objs[i]);
			objs[i]->type = type;
			hint = i + 1;
			return HBASE + 4 * i;
		}
	}
	w16_log("startwin: out of GDI objects\n");
	return 0;
}

static void
bm_free(b)
	struct bitmap *b;
{
	if (b) {
		free(b->s.pix);
		free(b);
	}
}

void
gobj_delete(h)
	u32 h;
{
	struct gobj *o = gobj(h, 0);
	int i;

	if (!o || o->stock)
		return;
	switch (o->type) {
	case OBJ_BITMAP:
		bm_free(o->u.bm);
		break;
	case OBJ_RGN:
		rgn_free(&o->u.rgn);
		break;
	case OBJ_PAL:
		free(o->u.pal);
		break;
	case OBJ_BRUSH:
		break;
	}
	i = ((h & 0xffff) - HBASE) >> 2;
	free(objs[i]);
	objs[i] = 0;
}

static struct bitmap *
bm_new(w, h, mono)
	int w, h, mono;
{
	struct bitmap *b = (struct bitmap *)calloc(1, sizeof *b);

	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	b->s.w = w;
	b->s.h = h;
	b->s.rowb = w;
	b->s.mono = mono;
	b->s.pix = (u8 *)calloc(1, w * h);
	b->bpp = mono ? 1 : 8;
	b->planes = 1;
	return b;
}

static u16
mkpen(style, width, color)
	int style, width;
	COLORREF color;
{
	u16 h = gobj_new(OBJ_PEN);
	struct gobj *o = gobj(h, 0);

	o->u.pen.style = style;
	o->u.pen.width = width;
	o->u.pen.color = color;
	return h;
}

static u16
mkbrush(style, color, hatch)
	int style, hatch;
	COLORREF color;
{
	static u8 hatches[6][8] = {
		{ 0, 0, 0, 0xff, 0, 0, 0, 0 },			/* HS_HORIZONTAL */
		{ 8, 8, 8, 8, 8, 8, 8, 8 },			/* HS_VERTICAL */
		{ 0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1 },		/* HS_FDIAGONAL */
		{ 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80 },		/* HS_BDIAGONAL */
		{ 8, 8, 8, 0xff, 8, 8, 8, 8 },			/* HS_CROSS */
		{ 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 } /* HS_DIAGCROSS */
	};
	u16 h = gobj_new(OBJ_BRUSH);
	struct gobj *o = gobj(h, 0);
	int i;

	o->u.brush.style = style;
	o->u.brush.color = color;
	o->u.brush.hatch = hatch;
	if (style == BS_HATCHED) {
		o->u.brush.monopat = 1;
		/* hatch lines in the brush colour (0), the rest background (1) */
		for (i = 0; i < 64; i++)
			o->u.brush.pat[i] = !(hatches[hatch % 6][i / 8] & (0x80 >> (i % 8)));
	}
	return h;
}

static u16
mkfont(lf)
	struct logfont *lf;
{
	u16 h = gobj_new(OBJ_FONT);
	struct gobj *o = gobj(h, 0);

	o->u.font.lf = *lf;
	o->u.font.bf = font_pick(lf);
	o->u.font.bold = lf->weight >= 600 && o->u.font.bf->f_weight < 600;
	return h;
}

/* the face of the list that stands for name, or 0 */
static char *
font_face(name)
	char *name;
{
	struct bfont *f;

	if (!name || !*name)
		return 0;
	for (f = fontlist; f->f_face; f++)
		if (w16_stricmp(f->f_face, name) == 0)
			return f->f_face;
	return 0;
}

/*
 * The font nearest what a LOGFONT asks: its face, else the face Windows
 * puts for it (FontSubstitutes, TrueType's bitmap stand-ins), else one
 * for its family and pitch; the built-in faces when Windows' fonts are
 * not there.  Then the nearest height, weight and slant of that face.
 */
struct bfont *
font_pick(lf)
	struct logfont *lf;
{
	static char *alias[][2] = {
		{ "Helv", "MS Sans Serif" }, { "Helvetica", "MS Sans Serif" }, { "Arial", "MS Sans Serif" },
		{ "Swiss", "MS Sans Serif" }, { "Tms Rmn", "MS Serif" }, { "Times", "MS Serif" },
		{ "Times New Roman", "MS Serif" }, { "Roman", "MS Serif" }, { "Courier New", "Courier" },
		{ "Modern", "Courier" }, { "Wingdings", "Symbol" },
		/* and the built-in faces for Windows' */
		{ "MS Sans Serif", "Helv" }, { "MS Serif", "Tms Rmn" }, { "System", "Helv" },
		{ "Small Fonts", "Helv" }, { "Fixedsys", "Courier" }, { "Terminal", "Courier" }, { 0, 0 }
	};
	struct bfont *f, *best = 0;
	char *face = 0, *try[6];
	int want, score, bs = 1 << 30, fixed, i, j, n = 0, fam = lf->pitchfam & 0xf0;

	fixed = (lf->pitchfam & 3) == 1 || fam == 0x30;
	try[n++] = lf->face;
	for (i = 0; alias[i][0] && n < 3; i++)
		if (!w16_stricmp(lf->face, alias[i][0]))
			try[n++] = alias[i][1];
	try[n++] = fixed ? "Courier" : fam == 0x10 ? "MS Serif" : "MS Sans Serif";
	try[n++] = fixed ? "Courier" : fam == 0x10 ? "Tms Rmn" : "Helv";
	for (i = 0; i < n && !face; i++) {
		face = font_face(try[i]);
		/* an alias of an alias: Arial -> MS Sans Serif -> Helv */
		for (j = 0; !face && alias[j][0]; j++)
			if (!w16_stricmp(try[i], alias[j][0]))
				face = font_face(alias[j][1]);
	}
	if (!face)
		face = fontlist[0].f_face;
	want = lf->height < 0 ? -lf->height : lf->height;
	if (want == 0)
		want = fixed ? 15 : 13;
	for (f = fontlist; f->f_face; f++) {
		int h;

		if (f->f_face != face && strcmp(f->f_face, face) != 0)
			continue;
		if (lf->height >= 0)
			h = f->f_height;
		else if (f->f_res)
			h = f->f_height - f->f_leading;
		else
			h = f->f_ascent + f->f_descent - (f->f_ascent + f->f_descent) / 6;
		/* nearest, a smaller one before a larger */
		score = (h > want ? (h - want) * 3 : (want - h) * 2) * 10;
		if ((f->f_weight >= 600) != (lf->weight >= 600))
			score += 15;
		if (!f->f_italic != !lf->italic)
			score += 5;
		if (score < bs) {
			bs = score;
			best = f;
		}
	}
	return best ? best : fontlist;
}

void
gdi_init()
{
	struct logfont lf;
	int i;

	pal_init();
	/* Windows' fonts from the user's SYSTEM directory, when there */
	{
		char host[1024];
		extern char sysdir[];

		font_init(dos_hostpath(sysdir, host, sizeof host, 0) == 0 ? host : (char *)0);
	}
	stockobj[WHITE_BRUSH] = mkbrush(BS_SOLID, RGB(255, 255, 255), 0);
	stockobj[LTGRAY_BRUSH] = mkbrush(BS_SOLID, RGB(192, 192, 192), 0);
	stockobj[GRAY_BRUSH] = mkbrush(BS_SOLID, RGB(128, 128, 128), 0);
	stockobj[DKGRAY_BRUSH] = mkbrush(BS_SOLID, RGB(64, 64, 64), 0);
	stockobj[BLACK_BRUSH] = mkbrush(BS_SOLID, RGB(0, 0, 0), 0);
	stockobj[NULL_BRUSH] = mkbrush(BS_NULL, 0, 0);
	stockobj[WHITE_PEN] = mkpen(PS_SOLID, 1, RGB(255, 255, 255));
	stockobj[BLACK_PEN] = mkpen(PS_SOLID, 1, 0);
	stockobj[NULL_PEN] = mkpen(PS_NULL, 1, 0);
	memset(&lf, 0, sizeof lf);
	lf.weight = 700;
	strcpy(lf.face, "System");
	lf.height = 16;
	stockobj[SYSTEM_FONT] = mkfont(&lf);
	stockobj[DEVICE_DEFAULT_FONT] = mkfont(&lf);
	lf.weight = 400;
	lf.pitchfam = 1;
	strcpy(lf.face, "Terminal");
	lf.height = 12;
	stockobj[OEM_FIXED_FONT] = mkfont(&lf);
	strcpy(lf.face, "Courier");
	lf.height = 14;
	stockobj[ANSI_FIXED_FONT] = mkfont(&lf);
	strcpy(lf.face, "Fixedsys");
	lf.height = 15;
	stockobj[SYSTEM_FIXED_FONT] = mkfont(&lf);
	lf.pitchfam = 2;
	strcpy(lf.face, "MS Sans Serif");
	lf.height = 12;
	stockobj[ANSI_VAR_FONT] = mkfont(&lf);
	stockobj[DEFAULT_PALETTE] = gobj_new(OBJ_PAL);
	gobj(stockobj[DEFAULT_PALETTE], 0)->u.pal = (struct palette *)calloc(1, sizeof(struct palette));
	gobj(stockobj[DEFAULT_PALETTE], 0)->u.pal->n = 20;
	for (i = 0; i < 20; i++) {
		struct palette *p = gobj(stockobj[DEFAULT_PALETTE], 0)->u.pal;
		int s = i < 10 ? i : 236 + i;

		memcpy(p->ent[i], syspal[s], 3);
		p->map[i] = s;
	}
	for (i = 0; i < NSTOCK; i++)
		if (stockobj[i])
			gobj(stockobj[i], 0)->stock = 1;
}

/* ---- DCs ---- */

struct dc *
dc_get(h)
	u32 h;
{
	struct gobj *o = gobj(h, OBJ_DC);

	return o ? o->u.dc : 0;
}

void
dc_reset(dc)
	struct dc *dc;
{
	struct dcstate *s = &dc->st;

	if (s->clip) {
		rgn_free(s->clip);
		free(s->clip);
	}
	memset(s, 0, sizeof *s);
	s->pen = stockobj[BLACK_PEN];
	s->brush = stockobj[WHITE_BRUSH];
	s->font = stockobj[SYSTEM_FONT];
	s->pal = stockobj[DEFAULT_PALETTE];
	s->text = 0;
	s->bk = RGB(255, 255, 255);
	s->bkmode = OPAQUE;
	s->rop2 = R2_COPYPEN;
	s->polyfill = 1;
	s->stretch = 1;
	s->mapmode = MM_TEXT;
	s->wex = s->wey = s->vex = s->vey = 1;
	dc->effok = 0;
}

u16
dc_new(kind)
	int kind;
{
	u16 h = gobj_new(kind == DCK_MEMORY ? OBJ_MEMDC : OBJ_DC);
	struct gobj *o = gobj(h, 0);
	struct dc *dc = (struct dc *)calloc(1, sizeof *dc);

	o->u.dc = dc;
	dc->kind = kind;
	dc->h = h;
	rgn_init(&dc->vis);
	rgn_init(&dc->eff);
	dc_reset(dc);
	return h;
}

void
dc_free(h)
	u32 h;
{
	struct gobj *o = gobj(h, OBJ_DC);
	struct dc *dc;
	struct bitmap *b;

	if (!o)
		return;
	dc = o->u.dc;
	if (dc->st.bitmap && gobj(dc->st.bitmap, OBJ_BITMAP)) {
		b = gobj(dc->st.bitmap, OBJ_BITMAP)->u.bm;
		if (b->seldc == (h & 0xffff))
			b->seldc = 0;
	}
	if (dc->kind == DCK_MEMORY && dc->s && !dc->st.bitmap)
		bm_free((struct bitmap *)dc->priv_bm);
	while (dc->nsaved > 0) {
		struct dcstate *s = dc->saved[--dc->nsaved];

		if (s->clip) {
			rgn_free(s->clip);
			free(s->clip);
		}
		free(s);
	}
	if (dc->st.clip) {
		rgn_free(dc->st.clip);
		free(dc->st.clip);
	}
	rgn_free(&dc->vis);
	rgn_free(&dc->eff);
	if (dc->paint) {
		rgn_free(dc->paint);
		free(dc->paint);
	}
	free(dc);
	o->type = 0;
	objs[((h & 0xffff) - HBASE) >> 2] = 0;
	free(o);
}

struct rgn *
dc_clip(dc)
	struct dc *dc;
{
	if (dc->hwnd && dc->epoch != vis_epoch)
		dc_refresh(dc);
	if (!dc->effok) {
		if (dc->st.clip) {
			struct rgn t;

			rgn_init(&t);
			rgn_copy(&t, dc->st.clip);
			rgn_offset(&t, dc->ox, dc->oy);
			rgn_and(&dc->eff, &dc->vis, &t);
			rgn_free(&t);
		} else
			rgn_copy(&dc->eff, &dc->vis);
		if (dc->paint)
			rgn_and(&dc->eff, &dc->eff, dc->paint);
		dc->effok = 1;
	}
	return &dc->eff;
}

/* a memory DC draws on its bitmap, or on a 1x1 mono one */
static void
memdc_target(dc)
	struct dc *dc;
{
	struct gobj *o = gobj(dc->st.bitmap, OBJ_BITMAP);
	struct rect r;

	if (o)
		dc->s = &o->u.bm->s;
	else {
		if (!dc->priv_bm)
			dc->priv_bm = (void *)bm_new(1, 1, 1);
		dc->s = &((struct bitmap *)dc->priv_bm)->s;
	}
	dc->ox = dc->oy = 0;
	r_set(&r, 0, 0, dc->s->w, dc->s->h);
	rgn_set(&dc->vis, &r);
	dc->effok = 0;
}

struct bfont *
font_of(dc)
	struct dc *dc;
{
	struct gobj *o = gobj(dc->st.font, OBJ_FONT);

	if (!o)
		o = gobj(stockobj[SYSTEM_FONT], OBJ_FONT);
	return o->u.font.bf;
}

static struct font *
fontobj(dc)
	struct dc *dc;
{
	struct gobj *o = gobj(dc->st.font, OBJ_FONT);

	if (!o)
		o = gobj(stockobj[SYSTEM_FONT], OBJ_FONT);
	return &o->u.font;
}

/* ---- coordinates ---- */

static int
muldiv(a, b, c)
	int a, b, c;
{
	long long p;

	if (c == 0)
		return -1;
	p = (long long)a * b;
	return (int)((p + (p >= 0 ? c / 2 : -c / 2) * ((c > 0) ? 1 : -1)) / c);
}

void
lp2dp(dc, x, y)
	struct dc *dc;
	int *x, *y;
{
	struct dcstate *s = &dc->st;

	if (s->mapmode != MM_TEXT) {
		*x = muldiv(*x - s->wox, s->vex, s->wex) + s->vox;
		*y = muldiv(*y - s->woy, s->vey, s->wey) + s->voy;
	} else {
		*x = *x - s->wox + s->vox;
		*y = *y - s->woy + s->voy;
	}
}

void
dp2lp(dc, x, y)
	struct dc *dc;
	int *x, *y;
{
	struct dcstate *s = &dc->st;

	if (s->mapmode != MM_TEXT) {
		*x = muldiv(*x - s->vox, s->wex, s->vex) + s->wox;
		*y = muldiv(*y - s->voy, s->wey, s->vey) + s->woy;
	} else {
		*x = *x - s->vox + s->wox;
		*y = *y - s->voy + s->woy;
	}
}

int
lx2dx(dc, n)
	struct dc *dc;
	int n;
{
	return dc->st.mapmode == MM_TEXT ? n : muldiv(n, dc->st.vex, dc->st.wex);
}

int
ly2dy(dc, n)
	struct dc *dc;
	int n;
{
	return dc->st.mapmode == MM_TEXT ? n : muldiv(n, dc->st.vey, dc->st.wey);
}

/* logical point to surface pixel */
static void
tosurf(dc, x, y)
	struct dc *dc;
	int *x, *y;
{
	lp2dp(dc, x, y);
	*x += dc->ox;
	*y += dc->oy;
}

/* a logical rectangle to surface pixels, ordered */
static void
rtosurf(dc, r, l, t, rr, b)
	struct dc *dc;
	struct rect *r;
	int l, t, rr, b;
{
	int x;

	tosurf(dc, &l, &t);
	tosurf(dc, &rr, &b);
	if (l > rr)
		x = l, l = rr, rr = x;
	if (t > b)
		x = t, t = b, b = x;
	r_set(r, l, t, rr, b);
}

#define	DC(h)	struct dc *dc = dc_get(h); if (!dc) return 0

/* ---- DC exports ---- */

static u32
g_CreateCompatibleDC(a)
	u32 *a;
{
	u16 h = dc_new(DCK_MEMORY);

	memdc_target(dc_get(h));
	return h;
}

static u32
g_CreateDC(a)
	u32 *a;
{
	char *drv = gptr(a[0]);
	u16 h;
	struct dc *dc;
	struct rect r;

	if (drv && w16_stricmp(drv, "DISPLAY") != 0) {
		/* printers: an information context that draws nowhere */
		h = dc_new(DCK_INFO);
		dc = dc_get(h);
		dc->s = &screen;
		rgn_init(&dc->vis);
		return h;
	}
	h = dc_new(DCK_SCREEN);
	dc = dc_get(h);
	dc->s = &screen;
	r_set(&r, 0, 0, screen.w, screen.h);
	rgn_set(&dc->vis, &r);
	return h;
}

static u32
g_DeleteDC(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);

	if (!dc)
		return 0;
	if (dc->kind == DCK_WINDOW || dc->kind == DCK_WINDOWNC) {
		user_releasedc(a[0]);
		return 1;
	}
	dc_free(a[0]);
	return 1;
}

static u32
g_SaveDC(a)
	u32 *a;
{
	struct dcstate *s;
	DC(a[0]);

	if (dc->nsaved >= 16)
		return 0;
	s = (struct dcstate *)malloc(sizeof *s);
	*s = dc->st;
	if (dc->st.clip) {
		s->clip = (struct rgn *)malloc(sizeof(struct rgn));
		rgn_init(s->clip);
		rgn_copy(s->clip, dc->st.clip);
	}
	dc->saved[dc->nsaved++] = s;
	return dc->nsaved;
}

static u32
g_RestoreDC(a)
	u32 *a;
{
	int n = (short)a[1];
	struct dcstate *s;
	DC(a[0]);

	if (n < 0)
		n = dc->nsaved + 1 + n;
	if (n < 1 || n > dc->nsaved)
		return 0;
	while (dc->nsaved > n) {
		s = dc->saved[--dc->nsaved];
		if (s->clip) {
			rgn_free(s->clip);
			free(s->clip);
		}
		free(s);
	}
	s = dc->saved[--dc->nsaved];
	if (dc->st.clip) {
		rgn_free(dc->st.clip);
		free(dc->st.clip);
	}
	dc->st = *s;
	free(s);
	dc->effok = 0;
	if (dc->kind == DCK_MEMORY)
		memdc_target(dc);
	return 1;
}

static u32
g_GetDeviceCaps(a)
	u32 *a;
{
	DC(a[0]);

	switch (a[1]) {
	case 0: return 0x300;		/* DRIVERVERSION */
	case 2: return 1;		/* TECHNOLOGY: raster display */
	case 4: return 208;		/* HORZSIZE mm */
	case 6: return 156;		/* VERTSIZE */
	case 8: return dc->kind == DCK_MEMORY ? dc->s->w : screen.w;	/* HORZRES */
	case 10: return dc->kind == DCK_MEMORY ? dc->s->h : screen.h;
	case 12: return dc->s && dc->s->mono ? 1 : 8;	/* BITSPIXEL */
	case 14: return 1;		/* PLANES */
	case 16: return 0;		/* NUMBRUSHES: -1 as unsigned */
	case 18: return 100;		/* NUMPENS */
	case 20: return 0;		/* NUMMARKERS */
	case 22: return bfonts_count();	/* NUMFONTS */
	case 24: return dc->s && dc->s->mono ? 2 : 20;	/* NUMCOLORS: the statics */
	case 26: return 0;		/* PDEVICESIZE */
	case 28: return 0x00ff;		/* CURVECAPS */
	case 30: return 0x00fe;		/* LINECAPS */
	case 32: return 0x00ff;		/* POLYGONALCAPS */
	case 34: return 0x0004 | 0x0002;	/* TEXTCAPS: stroke clip, char precision */
	case 36: return 1;		/* CLIPCAPS */
	case 38: return 0x0001 | 0x0008 | 0x0080 | 0x0100 | 0x0200 | 0x0800;	/* RASTERCAPS */
	case 40: return 36;		/* ASPECTX */
	case 42: return 36;		/* ASPECTY */
	case 44: return 51;		/* ASPECTXY */
	case 88: return 96;		/* LOGPIXELSX */
	case 90: return 96;		/* LOGPIXELSY */
	case 104: return 256;		/* SIZEPALETTE */
	case 106: return 20;		/* NUMRESERVED */
	case 108: return 18;		/* COLORRES */
	}
	return 0;
}

int
bfonts_count()
{
	int n = 0;

	while (fontlist[n].f_face)
		n++;
	return n;
}

static u32
g_SelectObject(a)
	u32 *a;
{
	struct gobj *o = gobj(a[1], 0);
	u16 old = 0;
	DC(a[0]);

	if (!o)
		return 0;
	switch (o->type) {
	case OBJ_PEN:
		old = dc->st.pen;
		dc->st.pen = a[1];
		break;
	case OBJ_BRUSH:
		old = dc->st.brush;
		dc->st.brush = a[1];
		break;
	case OBJ_FONT:
		old = dc->st.font;
		dc->st.font = a[1];
		break;
	case OBJ_BITMAP:
		if (dc->kind != DCK_MEMORY)
			return 0;
		if (o->u.bm->seldc && o->u.bm->seldc != (a[0] & 0xffff) && dc_get(o->u.bm->seldc))
			return 0;
		old = dc->st.bitmap;
		if (old && gobj(old, OBJ_BITMAP))
			gobj(old, OBJ_BITMAP)->u.bm->seldc = 0;
		dc->st.bitmap = a[1];
		o->u.bm->seldc = a[0];
		memdc_target(dc);
		if (!old)
			old = stockbitmap();
		break;
	case OBJ_RGN:
		/* SelectClipRgn, returning the region kind */
		if (!dc->st.clip) {
			dc->st.clip = (struct rgn *)malloc(sizeof(struct rgn));
			rgn_init(dc->st.clip);
		}
		rgn_copy(dc->st.clip, &o->u.rgn);
		dc->effok = 0;
		return rgn_kind(dc->st.clip);
	default:
		return 0;
	}
	return old;
}

/* the 1x1 mono bitmap a new memory DC holds, as a handle */
u16
stockbitmap()
{
	static u16 h;

	if (!h) {
		h = gobj_new(OBJ_BITMAP);
		gobj(h, 0)->u.bm = bm_new(1, 1, 1);
		gobj(h, 0)->stock = 1;
	}
	return h;
}

static u32
g_GetStockObject(a)
	u32 *a;
{
	return a[0] < NSTOCK ? stockobj[a[0]] : 0;
}

static u32
g_DeleteObject(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], 0);

	if (!o || o->stock)
		return o != 0;
	if (o->type == OBJ_DC || o->type == OBJ_MEMDC)
		return 0;
	if (o->type == OBJ_BITMAP && o->u.bm->seldc && dc_get(o->u.bm->seldc))
		return 0;
	gobj_delete(a[0]);
	return 1;
}

static u32
g_UnrealizeObject(a)
	u32 *a;
{
	return 1;
}

/* GetObject: the LOGPEN, LOGBRUSH, LOGFONT, BITMAP or palette count */
static u32
g_GetObject(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], 0);
	u32 d = lin(FPSEL(a[2]), FPOFF(a[2]));
	int n = (short)a[1], k;
	u8 buf[64];

	if (!o)
		return 0;
	memset(buf, 0, sizeof buf);
	switch (o->type) {
	case OBJ_PEN:
		WR16(buf, 0, o->u.pen.style);
		WR16(buf, 2, o->u.pen.width);
		WR32(buf, 6, o->u.pen.color);
		k = 10;
		break;
	case OBJ_BRUSH:
		WR16(buf, 0, o->u.brush.style);
		WR32(buf, 2, o->u.brush.color);
		WR16(buf, 6, o->u.brush.style == BS_PATTERN ? o->u.brush.bitmap : o->u.brush.hatch);
		k = 8;
		break;
	case OBJ_FONT:
		{
			struct logfont *lf = &o->u.font.lf;

			WR16(buf, 0, lf->height);
			WR16(buf, 2, lf->width);
			WR16(buf, 4, lf->escapement);
			WR16(buf, 6, lf->orientation);
			WR16(buf, 8, lf->weight);
			buf[10] = lf->italic;
			buf[11] = lf->underline;
			buf[12] = lf->strikeout;
			buf[13] = lf->charset;
			buf[14] = lf->outprec;
			buf[15] = lf->clipprec;
			buf[16] = lf->quality;
			buf[17] = lf->pitchfam;
			strncpy((char *)buf + 18, lf->face, 31);
			k = 50;
		}
		break;
	case OBJ_BITMAP:
		{
			struct bitmap *b = o->u.bm;

			WR16(buf, 0, 0);
			WR16(buf, 2, b->s.w);
			WR16(buf, 4, b->s.h);
			WR16(buf, 6, b->s.mono ? ((b->s.w + 15) / 16) * 2 : (b->s.w + 1) & ~1);
			buf[8] = 1;
			buf[9] = b->s.mono ? 1 : 8;
			k = 14;
		}
		break;
	case OBJ_PAL:
		WR16(buf, 0, o->u.pal->n);
		k = 2;
		break;
	default:
		return 0;
	}
	if (!d)
		return k;
	if (n > k)
		n = k;
	if (n < 0)
		n = 0;
	memcpy(M + d, buf, n);
	return n;
}

/* ---- pens, brushes, fonts ---- */

static u32 g_CreatePen(a) u32 *a; { return mkpen(a[0], (short)a[1], a[2]); }

static u32
g_CreatePenIndirect(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));

	return p ? mkpen(GW(p), (short)GW(p + 2), GL(p + 6)) : 0;
}

static u32 g_CreateSolidBrush(a) u32 *a; { return mkbrush(BS_SOLID, a[0], 0); }
static u32 g_CreateHatchBrush(a) u32 *a; { return mkbrush(BS_HATCHED, a[1], a[0]); }

/* a pattern brush from the top left 8x8 of a bitmap */
static u16
patbrush(hbm)
	u32 hbm;
{
	struct gobj *b = gobj(hbm, OBJ_BITMAP);
	u16 h;
	struct gobj *o;
	int x, y;

	if (!b)
		return 0;
	h = mkbrush(BS_PATTERN, 0, 0);
	o = gobj(h, 0);
	o->u.brush.monopat = b->u.bm->s.mono;
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++)
			o->u.brush.pat[y * 8 + x] = b->u.bm->s.pix[(y % b->u.bm->s.h) * b->u.bm->s.rowb +
			    (x % b->u.bm->s.w)];
	o->u.brush.bitmap = hbm;
	return h;
}

static u32 g_CreatePatternBrush(a) u32 *a; { return patbrush(a[0]); }

static u32 dib_brush();

static u32
g_CreateBrushIndirect(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));
	int style;

	if (!p)
		return 0;
	style = GW(p);
	if (style == BS_PATTERN)
		return patbrush(GW(p + 6));
	if (style == BS_DIBPATTERN)
		return dib_brush(GW(p + 6), GW(p + 2));
	return mkbrush(style, GL(p + 2), GW(p + 6));
}

static u32
g_CreateDIBPatternBrush(a)
	u32 *a;
{
	return dib_brush(a[0], a[1]);
}

static void
lfget(lf, p)
	struct logfont *lf;
	u32 p;
{
	memset(lf, 0, sizeof *lf);
	lf->height = (short)GW(p);
	lf->width = (short)GW(p + 2);
	lf->escapement = (short)GW(p + 4);
	lf->orientation = (short)GW(p + 6);
	lf->weight = (short)GW(p + 8);
	lf->italic = M[p + 10];
	lf->underline = M[p + 11];
	lf->strikeout = M[p + 12];
	lf->charset = M[p + 13];
	lf->outprec = M[p + 14];
	lf->clipprec = M[p + 15];
	lf->quality = M[p + 16];
	lf->pitchfam = M[p + 17];
	strncpy(lf->face, (char *)M + p + 18, 31);
}

static u32
g_CreateFontIndirect(a)
	u32 *a;
{
	struct logfont lf;
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));

	if (!p)
		return 0;
	lfget(&lf, p);
	return mkfont(&lf);
}

static u32
g_CreateFont(a)
	u32 *a;
{
	struct logfont lf;

	memset(&lf, 0, sizeof lf);
	lf.height = (short)a[0];
	lf.width = (short)a[1];
	lf.escapement = (short)a[2];
	lf.orientation = (short)a[3];
	lf.weight = (short)a[4];
	lf.italic = a[5];
	lf.underline = a[6];
	lf.strikeout = a[7];
	lf.charset = a[8];
	lf.outprec = a[9];
	lf.clipprec = a[10];
	lf.quality = a[11];
	lf.pitchfam = a[12];
	strncpy(lf.face, STR(a[13]), 31);
	return mkfont(&lf);
}

/* ---- attributes ---- */

static u32 g_SetTextColor(a) u32 *a; { u32 o; DC(a[0]); o = dc->st.text; dc->st.text = a[1]; return o; }
static u32 g_GetTextColor(a) u32 *a; { DC(a[0]); return dc->st.text; }
static u32 g_SetBkColor(a) u32 *a; { u32 o; DC(a[0]); o = dc->st.bk; dc->st.bk = a[1]; return o; }
static u32 g_GetBkColor(a) u32 *a; { DC(a[0]); return dc->st.bk; }
static u32 g_SetBkMode(a) u32 *a; { u32 o; DC(a[0]); o = dc->st.bkmode; dc->st.bkmode = a[1]; return o; }
static u32 g_GetBkMode(a) u32 *a; { DC(a[0]); return dc->st.bkmode; }
static u32 g_SetROP2(a) u32 *a; { u32 o; DC(a[0]); o = dc->st.rop2; dc->st.rop2 = a[1]; return o; }
static u32 g_GetROP2(a) u32 *a; { DC(a[0]); return dc->st.rop2; }
static u32 g_SetPolyFillMode(a) u32 *a; { u32 o; DC(a[0]); o = dc->st.polyfill; dc->st.polyfill = a[1]; return o; }
static u32 g_GetPolyFillMode(a) u32 *a; { DC(a[0]); return dc->st.polyfill; }
static u32 g_SetStretchBltMode(a) u32 *a; { u32 o; DC(a[0]); o = dc->st.stretch; dc->st.stretch = a[1]; return o; }
static u32 g_GetStretchBltMode(a) u32 *a; { DC(a[0]); return dc->st.stretch; }
static u32 g_SetTextAlign(a) u32 *a; { u32 o; DC(a[0]); o = dc->st.align; dc->st.align = a[1]; return o; }
static u32 g_GetTextAlign(a) u32 *a; { DC(a[0]); return dc->st.align; }
static u32 g_SetTextCharacterExtra(a) u32 *a; { u32 o; DC(a[0]); o = dc->st.extra; dc->st.extra = (short)a[1]; return o; }
static u32 g_GetTextCharacterExtra(a) u32 *a; { DC(a[0]); return dc->st.extra; }

static u32
g_SetTextJustification(a)
	u32 *a;
{
	DC(a[0]);
	dc->st.breakext = (short)a[1];
	dc->st.breakcnt = (short)a[2];
	return 1;
}

static u32
g_SetBrushOrg(a)
	u32 *a;
{
	u32 o;
	DC(a[0]);

	o = FP(dc->st.by, dc->st.bx);
	dc->st.bx = (short)a[1];
	dc->st.by = (short)a[2];
	return o;
}

static u32 g_GetBrushOrg(a) u32 *a; { DC(a[0]); return FP(dc->st.by, dc->st.bx); }

static u32
g_GetDCOrg(a)
	u32 *a;
{
	DC(a[0]);
	return FP(dc->oy, dc->ox);
}

static u32
g_GetCurrentPosition(a)
	u32 *a;
{
	DC(a[0]);
	return FP(dc->st.cury, dc->st.curx);
}

static u32
g_GetNearestColor(a)
	u32 *a;
{
	DC(a[0]);
	return dc->s && dc->s->mono ? (pal_mono(a[1]) ? RGB(255, 255, 255) : 0) : idxcolor(pal_index(a[1]));
}

/* ---- mapping ---- */

static void
setmode(dc, m)
	struct dc *dc;
	int m;
{
	struct dcstate *s = &dc->st;

	s->mapmode = m;
	switch (m) {
	case MM_TEXT:
		s->wex = s->wey = s->vex = s->vey = 1;
		break;
	case MM_LOMETRIC: s->wex = s->wey = 254; s->vex = 96; s->vey = -96; break;
	case MM_HIMETRIC: s->wex = s->wey = 2540; s->vex = 96; s->vey = -96; break;
	case MM_LOENGLISH: s->wex = s->wey = 100; s->vex = 96; s->vey = -96; break;
	case MM_HIENGLISH: s->wex = s->wey = 1000; s->vex = 96; s->vey = -96; break;
	case MM_TWIPS: s->wex = s->wey = 1440; s->vex = 96; s->vey = -96; break;
	case MM_ISOTROPIC: case MM_ANISOTROPIC:
		break;
	}
}

static void
fixiso(dc)
	struct dc *dc;
{
	struct dcstate *s = &dc->st;
	long long xs, ys;

	if (s->mapmode != MM_ISOTROPIC || !s->wex || !s->wey)
		return;
	/* the smaller scale for both, keeping signs */
	xs = (long long)(s->vex < 0 ? -s->vex : s->vex) * (s->wey < 0 ? -s->wey : s->wey);
	ys = (long long)(s->vey < 0 ? -s->vey : s->vey) * (s->wex < 0 ? -s->wex : s->wex);
	if (xs > ys)
		s->vex = (int)((long long)s->vex * ys / xs);
	else if (ys > xs)
		s->vey = (int)((long long)s->vey * xs / ys);
}

static u32
g_SetMapMode(a)
	u32 *a;
{
	u32 o;
	DC(a[0]);

	o = dc->st.mapmode;
	if (a[1] >= MM_TEXT && a[1] <= MM_ANISOTROPIC)
		setmode(dc, a[1]);
	return o;
}

static u32 g_GetMapMode(a) u32 *a; { DC(a[0]); return dc->st.mapmode; }

#define	ORG(f, x, y) do { u32 o = FP(dc->st.y, dc->st.x); dc->st.x = (short)a[1]; dc->st.y = (short)a[2]; return o; } while (0)

static u32 g_SetWindowOrg(a) u32 *a; { DC(a[0]); ORG(0, wox, woy); }
static u32 g_SetViewportOrg(a) u32 *a; { DC(a[0]); ORG(0, vox, voy); }
static u32 g_GetWindowOrg(a) u32 *a; { DC(a[0]); return FP(dc->st.woy, dc->st.wox); }
static u32 g_GetViewportOrg(a) u32 *a; { DC(a[0]); return FP(dc->st.voy, dc->st.vox); }
static u32 g_GetWindowExt(a) u32 *a; { DC(a[0]); return FP(dc->st.wey, dc->st.wex); }
static u32 g_GetViewportExt(a) u32 *a; { DC(a[0]); return FP(dc->st.vey, dc->st.vex); }

static u32
g_SetWindowExt(a)
	u32 *a;
{
	u32 o;
	DC(a[0]);

	o = FP(dc->st.wey, dc->st.wex);
	if (dc->st.mapmode != MM_ISOTROPIC && dc->st.mapmode != MM_ANISOTROPIC)
		return o;
	if ((short)a[1] == 0 || (short)a[2] == 0)
		return 0;
	dc->st.wex = (short)a[1];
	dc->st.wey = (short)a[2];
	fixiso(dc);
	return o;
}

static u32
g_SetViewportExt(a)
	u32 *a;
{
	u32 o;
	DC(a[0]);

	o = FP(dc->st.vey, dc->st.vex);
	if (dc->st.mapmode != MM_ISOTROPIC && dc->st.mapmode != MM_ANISOTROPIC)
		return o;
	if ((short)a[1] == 0 || (short)a[2] == 0)
		return 0;
	dc->st.vex = (short)a[1];
	dc->st.vey = (short)a[2];
	fixiso(dc);
	return o;
}

static u32
g_OffsetWindowOrg(a)
	u32 *a;
{
	u32 o;
	DC(a[0]);

	o = FP(dc->st.woy, dc->st.wox);
	dc->st.wox += (short)a[1];
	dc->st.woy += (short)a[2];
	return o;
}

static u32
g_OffsetViewportOrg(a)
	u32 *a;
{
	u32 o;
	DC(a[0]);

	o = FP(dc->st.voy, dc->st.vox);
	dc->st.vox += (short)a[1];
	dc->st.voy += (short)a[2];
	return o;
}

static u32
g_ScaleViewportExt(a)
	u32 *a;
{
	u32 o;
	DC(a[0]);

	o = FP(dc->st.vey, dc->st.vex);
	if (dc->st.mapmode != MM_ISOTROPIC && dc->st.mapmode != MM_ANISOTROPIC)
		return o;
	if ((short)a[2] && (short)a[4]) {
		dc->st.vex = dc->st.vex * (short)a[1] / (short)a[2];
		dc->st.vey = dc->st.vey * (short)a[3] / (short)a[4];
		fixiso(dc);
	}
	return o;
}

static u32
g_ScaleWindowExt(a)
	u32 *a;
{
	u32 o;
	DC(a[0]);

	o = FP(dc->st.wey, dc->st.wex);
	if (dc->st.mapmode != MM_ISOTROPIC && dc->st.mapmode != MM_ANISOTROPIC)
		return o;
	if ((short)a[2] && (short)a[4]) {
		dc->st.wex = dc->st.wex * (short)a[1] / (short)a[2];
		dc->st.wey = dc->st.wey * (short)a[3] / (short)a[4];
		fixiso(dc);
	}
	return o;
}

static u32
g_LPtoDP(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	int i, x, y;
	DC(a[0]);

	for (i = 0; p && i < (short)a[2]; i++, p += 4) {
		x = (short)GW(p);
		y = (short)GW(p + 2);
		lp2dp(dc, &x, &y);
		PW(p, x);
		PW(p + 2, y);
	}
	return 1;
}

static u32
g_DPtoLP(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	int i, x, y;
	DC(a[0]);

	for (i = 0; p && i < (short)a[2]; i++, p += 4) {
		x = (short)GW(p);
		y = (short)GW(p + 2);
		dp2lp(dc, &x, &y);
		PW(p, x);
		PW(p + 2, y);
	}
	return 1;
}

static u32
g_MulDiv(a)
	u32 *a;
{
	int r;

	if ((short)a[2] == 0)
		return 0xffff8000;
	r = muldiv((short)a[0], (short)a[1], (short)a[2]);
	if (r > 32767 || r < -32768)
		return 0x8000;
	return r & 0xffff;
}

/* ---- drawing ---- */

static u32
g_MoveTo(a)
	u32 *a;
{
	u32 o;
	DC(a[0]);

	o = FP(dc->st.cury, dc->st.curx);
	dc->st.curx = (short)a[1];
	dc->st.cury = (short)a[2];
	return o;
}

static u32
g_LineTo(a)
	u32 *a;
{
	int x0, y0, x1 = (short)a[1], y1 = (short)a[2];
	DC(a[0]);

	x0 = dc->st.curx;
	y0 = dc->st.cury;
	dc->st.curx = x1;
	dc->st.cury = y1;
	tosurf(dc, &x0, &y0);
	tosurf(dc, &x1, &y1);
	d_line(dc, x0, y0, x1, y1);
	return 1;
}

/* the pen's width in device units, for the inside frame */
static int
penw(dc)
	struct dc *dc;
{
	struct gobj *o = gobj(dc->st.pen, OBJ_PEN);

	if (!o || o->u.pen.style == PS_NULL)
		return 0;
	return o->u.pen.width > 1 ? lx2dx(dc, o->u.pen.width) : 1;
}

static void
outline(dc, r)
	struct dc *dc;
	struct rect *r;
{
	struct gobj *o = gobj(dc->st.pen, OBJ_PEN);
	int w = penw(dc);
	struct rect e;

	if (!w || R_EMPTY(r))
		return;
	if (o->u.pen.style == PS_SOLID || o->u.pen.style == PS_INSIDEFRAME || w > 1) {
		int idx = dc->s->mono ? pal_mono(o->u.pen.color) : pal_index(o->u.pen.color);

		if (dc->st.rop2 == R2_COPYPEN) {
			r_set(&e, r->l, r->t, r->r, r->t + w); d_fillcolor(dc, &e, idx);
			r_set(&e, r->l, r->b - w, r->r, r->b); d_fillcolor(dc, &e, idx);
			r_set(&e, r->l, r->t, r->l + w, r->b); d_fillcolor(dc, &e, idx);
			r_set(&e, r->r - w, r->t, r->r, r->b); d_fillcolor(dc, &e, idx);
			return;
		}
	}
	d_line(dc, r->l, r->t, r->r - 1, r->t);
	d_line(dc, r->r - 1, r->t, r->r - 1, r->b - 1);
	d_line(dc, r->r - 1, r->b - 1, r->l, r->b - 1);
	d_line(dc, r->l, r->b - 1, r->l, r->t);
}

static u32
g_Rectangle(a)
	u32 *a;
{
	struct rect r, in;
	int w;
	DC(a[0]);

	rtosurf(dc, &r, (short)a[1], (short)a[2], (short)a[3], (short)a[4]);
	w = penw(dc);
	in = r;
	if (w) {
		in.l += w;
		in.t += w;
		in.r -= w;
		in.b -= w;
	}
	d_fill(dc, &in, dc->st.brush, d_fillrop(dc));
	outline(dc, &r);
	return 1;
}

static u32
g_RoundRect(a)
	u32 *a;
{
	return g_Rectangle(a);
}

static u32
g_Ellipse(a)
	u32 *a;
{
	struct rect r;
	struct gobj *b;
	DC(a[0]);

	rtosurf(dc, &r, (short)a[1], (short)a[2], (short)a[3], (short)a[4]);
	b = gobj(dc->st.brush, OBJ_BRUSH);
	d_ellipse(dc, &r, !b || b->u.brush.style != BS_NULL, 1);
	return 1;
}

/* the points of a guest POINT array, mapped */
static int *
points(dc, p, n)
	struct dc *dc;
	u32 p;
	int n;
{
	int *v = (int *)malloc(sizeof(int) * 2 * (n + 1)), i;

	for (i = 0; i < n; i++) {
		v[2 * i] = (short)GW(p + 4 * i);
		v[2 * i + 1] = (short)GW(p + 4 * i + 2);
		tosurf(dc, &v[2 * i], &v[2 * i + 1]);
	}
	return v;
}

static u32
g_Polygon(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	int n = (short)a[2], *v;
	struct gobj *b;
	DC(a[0]);

	if (!p || n < 2)
		return 0;
	v = points(dc, p, n);
	b = gobj(dc->st.brush, OBJ_BRUSH);
	d_poly(dc, v, n, !b || b->u.brush.style != BS_NULL, 1);
	if (b && b->u.brush.style == BS_NULL)
		d_line(dc, v[2 * n - 2], v[2 * n - 1], v[0], v[1]);
	free(v);
	return 1;
}

static u32
g_Polyline(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	int n = (short)a[2], *v;
	DC(a[0]);

	if (!p || n < 2)
		return 0;
	v = points(dc, p, n);
	d_poly(dc, v, n, 0, 1);
	free(v);
	return 1;
}

static u32
g_PolyPolygon(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1])), c = lin(FPSEL(a[2]), FPOFF(a[2]));
	int k, n;
	u32 b[3];

	if (!p || !c)
		return 0;
	for (k = 0; k < (short)a[3]; k++) {
		n = GW(c + 2 * k);
		b[0] = a[0];
		b[1] = FP(FPSEL(a[1]), FPOFF(a[1]) + (p - lin(FPSEL(a[1]), FPOFF(a[1]))));
		b[2] = n;
		g_Polygon(b);
		p += 4 * n;
		a[1] += 4 * n;
	}
	return 1;
}

/* points along an ellipse from one radial to another (counterclockwise) */
static int
arcpts(r, x1, y1, x2, y2, out, max)
	struct rect *r;
	int x1, y1, x2, y2, *out, max;
{
	double cx = (r->l + r->r) / 2.0, cy = (r->t + r->b) / 2.0;
	double rx = (r->r - r->l) / 2.0, ry = (r->b - r->t) / 2.0;
	double a1 = atan2(-(y1 - cy), x1 - cx), a2 = atan2(-(y2 - cy), x2 - cx), t;
	int n = 0, i, steps;

	if (a2 <= a1)
		a2 += 2 * 3.14159265358979;
	steps = (int)((a2 - a1) * (rx > ry ? rx : ry) / 3) + 2;
	if (steps > max - 1)
		steps = max - 1;
	for (i = 0; i <= steps; i++) {
		t = a1 + (a2 - a1) * i / steps;
		out[2 * n] = (int)(cx + rx * cos(t) + 0.5);
		out[2 * n + 1] = (int)(cy - ry * sin(t) + 0.5);
		n++;
	}
	return n;
}

static u32
arcish(a, kind)
	u32 *a;
	int kind;		/* 0 arc, 1 pie, 2 chord */
{
	struct rect r;
	int x1 = (short)a[5], y1 = (short)a[6], x2 = (short)a[7], y2 = (short)a[8], *v, n;
	struct gobj *b;
	DC(a[0]);

	rtosurf(dc, &r, (short)a[1], (short)a[2], (short)a[3], (short)a[4]);
	tosurf(dc, &x1, &y1);
	tosurf(dc, &x2, &y2);
	v = (int *)malloc(sizeof(int) * 2 * 2050);
	n = arcpts(&r, x1, y1, x2, y2, v, 2048);
	if (kind == 1) {
		v[2 * n] = (r.l + r.r) / 2;
		v[2 * n + 1] = (r.t + r.b) / 2;
		n++;
	}
	b = gobj(dc->st.brush, OBJ_BRUSH);
	if (kind == 0)
		d_poly(dc, v, n, 0, 1);
	else
		d_poly(dc, v, n, !b || b->u.brush.style != BS_NULL, 1);
	free(v);
	return 1;
}

static u32 g_Arc(a) u32 *a; { return arcish(a, 0); }
static u32 g_Pie(a) u32 *a; { return arcish(a, 1); }
static u32 g_Chord(a) u32 *a; { return arcish(a, 2); }

static u32
g_PatBlt(a)
	u32 *a;
{
	struct rect r;
	int x = (short)a[1], y = (short)a[2], w = (short)a[3], h = (short)a[4];
	DC(a[0]);

	rtosurf(dc, &r, x, y, x + w, y + h);
	d_fill(dc, &r, dc->st.brush, a[5] >> 16 & 0xff);
	return 1;
}

static u32
g_BitBlt(a)
	u32 *a;
{
	struct dc *sdc = dc_get(a[5]);
	int dx = (short)a[1], dy = (short)a[2], w = (short)a[3], h = (short)a[4];
	int sx = (short)a[6], sy = (short)a[7], x2, y2;
	DC(a[0]);

	x2 = dx + w;
	y2 = dy + h;
	tosurf(dc, &dx, &dy);
	tosurf(dc, &x2, &y2);
	if (sdc) {
		tosurf(sdc, &sx, &sy);
		if (sdc->kind == DCK_INFO)
			return 1;
	}
	if (dc->kind == DCK_INFO)
		return 1;
	if (x2 < dx) {
		int t = dx;

		dx = x2;
		x2 = t;
	}
	if (y2 < dy) {
		int t = dy;

		dy = y2;
		y2 = t;
	}
	d_blt(dc, dx, dy, x2 - dx, y2 - dy, sdc, sx, sy, a[8]);
	return 1;
}

static u32
g_StretchBlt(a)
	u32 *a;
{
	struct dc *sdc = dc_get(a[5]);
	int dx = (short)a[1], dy = (short)a[2], dw = (short)a[3], dh = (short)a[4];
	int sx = (short)a[6], sy = (short)a[7], sw = (short)a[8], sh = (short)a[9];
	int x2, y2, sx2, sy2;
	DC(a[0]);

	if (!sdc || dc->kind == DCK_INFO)
		return 1;
	x2 = dx + dw;
	y2 = dy + dh;
	sx2 = sx + sw;
	sy2 = sy + sh;
	tosurf(dc, &dx, &dy);
	tosurf(dc, &x2, &y2);
	tosurf(sdc, &sx, &sy);
	tosurf(sdc, &sx2, &sy2);
	d_stretch(dc, dx, dy, x2 - dx, y2 - dy, sdc, sx, sy, sx2 - sx, sy2 - sy, a[10]);
	return 1;
}

static u32
g_SetPixel(a)
	u32 *a;
{
	int x = (short)a[1], y = (short)a[2], i;
	DC(a[0]);

	tosurf(dc, &x, &y);
	i = dc->s->mono ? pal_mono(a[3]) : pal_index(a[3]);
	d_pixel(dc, x, y, i);
	return dc->s->mono ? (i ? 0xffffff : 0) : idxcolor(i);
}

static u32
g_GetPixel(a)
	u32 *a;
{
	int x = (short)a[1], y = (short)a[2], v;
	DC(a[0]);

	tosurf(dc, &x, &y);
	if (!rgn_ptin(dc_clip(dc), x, y))
		return 0xffffffff;
	v = dc->s->pix[y * dc->s->rowb + x];
	return dc->s->mono ? (v ? 0xffffff : 0) : idxcolor(v);
}

/* FloodFill/ExtFloodFill: a scanline fill with the brush */
static u32
flood(dc, x, y, c, surface)
	struct dc *dc;
	int x, y, surface;
	COLORREF c;
{
	struct surf *s = dc->s;
	struct rgn *g = dc_clip(dc);
	int idx = s->mono ? pal_mono(c) : pal_index(c), *stack, sp = 0, max = 65536, l, r, t;
	u8 *mark;
	struct rect rr;

	if (!rgn_ptin(g, x, y))
		return 0;
	mark = (u8 *)calloc(1, s->w * s->h);
	stack = (int *)malloc(sizeof(int) * 2 * max);
#define	IN(px, py)	(rgn_ptin(g, px, py) && !mark[(py) * s->w + (px)] && \
	(surface ? s->pix[(py) * s->rowb + (px)] == idx : s->pix[(py) * s->rowb + (px)] != idx))
	if (!IN(x, y)) {
		free(mark);
		free(stack);
		return 0;
	}
	stack[sp++] = x;
	stack[sp++] = y;
	while (sp > 0) {
		y = stack[--sp];
		x = stack[--sp];
		if (!IN(x, y))
			continue;
		for (l = x; l > 0 && IN(l - 1, y); l--)
			;
		for (r = x; r + 1 < s->w && IN(r + 1, y); r++)
			;
		for (t = l; t <= r; t++)
			mark[y * s->w + t] = 1;
		r_set(&rr, l, y, r + 1, y + 1);
		for (t = l; t <= r && sp < 2 * max - 4; t++) {
			if (y > 0 && IN(t, y - 1)) {
				stack[sp++] = t;
				stack[sp++] = y - 1;
			}
			if (y + 1 < s->h && IN(t, y + 1)) {
				stack[sp++] = t;
				stack[sp++] = y + 1;
			}
		}
		d_fill(dc, &rr, dc->st.brush, 0xf0);
	}
#undef IN
	free(mark);
	free(stack);
	return 1;
}

static u32
g_FloodFill(a)
	u32 *a;
{
	int x = (short)a[1], y = (short)a[2];
	DC(a[0]);

	tosurf(dc, &x, &y);
	return flood(dc, x, y, a[3], 0);
}

static u32
g_ExtFloodFill(a)
	u32 *a;
{
	int x = (short)a[1], y = (short)a[2];
	DC(a[0]);

	tosurf(dc, &x, &y);
	return flood(dc, x, y, a[3], a[4] == 1);
}

/* ---- text ---- */

/*
 * Text at logical (x, y) by the alignment; the cell's top left in
 * surface pixels back in *sx, *sy and the width drawn in *w.
 */
static void
place(dc, x, y, s, n, dx, sxp, syp, wp)
	struct dc *dc;
	int x, y, n, *dx, *sxp, *syp, *wp;
	char *s;
{
	struct bfont *f = font_of(dc);
	struct font *fo = fontobj(dc);
	int w = 0, i;

	if (dx)
		for (i = 0; i < n; i++)
			w += dx[i];
	else
		w = text_width(f, s, n) + n * (fo->bold + dc->st.extra);
	if (dc->st.align & TA_UPDATECP) {
		x = dc->st.curx;
		y = dc->st.cury;
	}
	tosurf(dc, &x, &y);
	if ((dc->st.align & TA_CENTER) == TA_CENTER)
		x -= w / 2;
	else if (dc->st.align & TA_RIGHT)
		x -= w;
	if ((dc->st.align & TA_BASELINE) == TA_BASELINE)
		y -= f->f_ascent;
	else if (dc->st.align & TA_BOTTOM)
		y -= f->f_height;
	*sxp = x;
	*syp = y;
	*wp = w;
}

/* draw at a surface position; opaque fills the cells, or *opq when given */
void
text_draw(dc, x, y, s, n, clip, opq, dx)
	struct dc *dc;
	int x, y, n, *dx;
	char *s;
	struct rect *clip, *opq;
{
	struct bfont *f = font_of(dc);
	struct font *fo = fontobj(dc);
	struct rect r;
	struct rgn save;
	int w, i, *adv = dx, own = 0;
	extern void glyphs();

	if (!adv && (dc->st.extra || dc->st.breakext)) {
		adv = (int *)malloc(sizeof(int) * (n + 1));
		own = 1;
		for (i = 0; i < n; i++) {
			int c = (u8)s[i];

			adv[i] = text_width(f, s + i, 1) + fo->bold + dc->st.extra;
			if (c == ' ' && dc->st.breakcnt > 0)
				adv[i] += dc->st.breakext / dc->st.breakcnt;
		}
	}
	for (w = 0, i = 0; i < n; i++)
		w += adv ? adv[i] : text_width(f, s + i, 1) + fo->bold;
	if (clip) {
		rgn_init(&save);
		rgn_copy(&save, dc_clip(dc));
		rgn_andrect(&dc->eff, clip);
	}
	if (opq) {
		r = *opq;
		d_fillcolor(dc, &r, dc->s->mono ? pal_mono(dc->st.bk) : pal_index(dc->st.bk));
	} else if (dc->st.bkmode == OPAQUE) {
		r_set(&r, x, y, x + w, y + f->f_height);
		d_fillcolor(dc, &r, dc->s->mono ? pal_mono(dc->st.bk) : pal_index(dc->st.bk));
	}
	glyphs(dc, f, x, y, s, n, adv, fo->bold, fo->lf.underline, fo->lf.strikeout);
	if (clip) {
		rgn_copy(&dc->eff, &save);
		rgn_free(&save);
	}
	if (own)
		free(adv);
}

static u32
g_TextOut(a)
	u32 *a;
{
	char *s = gptr(a[3]);
	int n = (short)a[4], x, y, w;
	DC(a[0]);

	if (!s || n <= 0)
		return 1;
	place(dc, (short)a[1], (short)a[2], s, n, (int *)0, &x, &y, &w);
	text_draw(dc, x, y, s, n, (struct rect *)0, (struct rect *)0, (int *)0);
	if (dc->st.align & TA_UPDATECP) {
		int ex = x + w - dc->ox, ey = 0;

		dp2lp(dc, &ex, &ey);
		dc->st.curx = ex;
	}
	return 1;
}

#define	ETO_OPAQUE	2
#define	ETO_CLIPPED	4

static u32
g_ExtTextOut(a)
	u32 *a;
{
	char *s = gptr(a[5]);
	int n = (short)a[6], x, y, w, *dx = 0, i;
	u32 rp = lin(FPSEL(a[4]), FPOFF(a[4])), dp = lin(FPSEL(a[7]), FPOFF(a[7]));
	struct rect r, *clip = 0, *opq = 0;
	DC(a[0]);

	if (rp) {
		struct rect lr;

		r_get(&lr, rp);
		rtosurf(dc, &r, lr.l, lr.t, lr.r, lr.b);
		if (a[3] & ETO_CLIPPED)
			clip = &r;
		if (a[3] & ETO_OPAQUE)
			opq = &r;
	}
	if (n < 0)
		n = 0;
	if (dp && n) {
		dx = (int *)malloc(sizeof(int) * n);
		for (i = 0; i < n; i++)
			dx[i] = lx2dx(dc, (short)GW(dp + 2 * i));
	}
	if (!s)
		n = 0;
	place(dc, (short)a[1], (short)a[2], s ? s : "", n, dx, &x, &y, &w);
	/* ETO_OPAQUE: the whole rectangle in the background colour, whatever the text covers */
	if (opq) {
		d_fillcolor(dc, opq, dc->s->mono ? pal_mono(dc->st.bk) : pal_index(dc->st.bk));
		opq = 0;
	}
	if (n)
		text_draw(dc, x, y, s ? s : "", n, clip, opq, dx);
	if (dx)
		free(dx);
	return 1;
}

static u32
g_GetTextExtent(a)
	u32 *a;
{
	char *s = gptr(a[1]);
	int n = (short)a[2], w, h;
	struct bfont *f;
	DC(a[0]);

	f = font_of(dc);
	w = s && n > 0 ? text_width(f, s, n) + n * (fontobj(dc)->bold + dc->st.extra) : 0;
	h = f->f_height;
	if (dc->st.mapmode != MM_TEXT) {
		w = muldiv(w, dc->st.wex, dc->st.vex);
		h = muldiv(h, dc->st.wey, dc->st.vey);
		if (w < 0) w = -w;
		if (h < 0) h = -h;
	}
	return FP(h, w);
}

static u32
g_GetTextExtentPoint(a)
	u32 *a;
{
	u32 r = g_GetTextExtent(a), p = lin(FPSEL(a[3]), FPOFF(a[3]));

	if (p) {
		PW(p, r);
		PW(p + 2, r >> 16);
	}
	return 1;
}

static u32
g_GetTextMetrics(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	struct bfont *f;
	struct font *fo;
	int lead;
	DC(a[0]);

	if (!p)
		return 0;
	f = font_of(dc);
	fo = fontobj(dc);
	lead = f->f_res ? f->f_leading : f->f_height > 14 ? 3 : f->f_height > 10 ? 2 : 1;
	PW(p, f->f_height);
	PW(p + 2, f->f_ascent);
	PW(p + 4, f->f_descent);
	PW(p + 6, lead);		/* internal leading */
	PW(p + 8, f->f_extlead);	/* external leading */
	PW(p + 10, f->f_avgw + fo->bold);
	PW(p + 12, f->f_maxw + fo->bold);
	PW(p + 14, fo->bold || f->f_weight >= 600 ? 700 : 400);
	PB(p + 16, fo->lf.italic);
	PB(p + 17, fo->lf.underline);
	PB(p + 18, fo->lf.strikeout);
	PB(p + 19, f->f_first);
	PB(p + 20, f->f_last);
	PB(p + 21, f->f_res ? f->f_default : '?');
	PB(p + 22, f->f_res ? f->f_break : ' ');
	/* pitch and family: bit 0 set is variable pitch, as Windows has it */
	PB(p + 23, (f->f_pitch ? 0 : 1) | f->f_family);
	PB(p + 24, f->f_charset);	/* ANSI_CHARSET mostly */
	PW(p + 25, fo->bold);		/* overhang */
	PW(p + 27, 96);
	PW(p + 29, 96);
	return 1;
}

static u32
g_GetTextFace(a)
	u32 *a;
{
	char *d = gptr(a[2]), *face;
	int n = (short)a[1];
	struct font *fo;
	DC(a[0]);

	fo = fontobj(dc);
	face = fo->lf.face[0] ? fo->lf.face : font_of(dc)->f_face;
	if (!d || n <= 0)
		return strlen(face);
	strncpy(d, face, n - 1);
	d[n - 1] = 0;
	return strlen(d);
}

static u32
g_GetCharWidth(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3]));
	struct bfont *f;
	int c;
	char ch;
	DC(a[0]);

	f = font_of(dc);
	for (c = a[1]; p && c <= (int)a[2]; c++, p += 2) {
		ch = c;
		PW(p, text_width(f, &ch, 1) + fontobj(dc)->bold);
	}
	return 1;
}

/* EnumFonts/EnumFontFamilies: each built-in face and size */
static u32
enumfonts(a, fam)
	u32 *a;
	int fam;
{
	char *want = gptr(a[1]);
	struct bfont *f;
	u16 lf, tm;
	u32 r = 1, lfa, tma, dsave;
	int i, seen;

	lf = g_alloc(GMEM_ZEROINIT, 128, 0);
	tm = g_alloc(GMEM_ZEROINIT, 64, 0);
	lfa = sel_base(lf);
	tma = sel_base(tm);
	for (f = fontlist; f->f_face && r; f++) {
		/* the built-in fonts only stand in when Windows' are not there */
		if (!f->f_res && fontlist[0].f_res)
			break;
		if (want && *want && w16_stricmp(want, f->f_face) != 0)
			continue;
		if (!want || !*want) {
			/* faces only: the first of each */
			for (seen = 0, i = 0; &fontlist[i] != f; i++)
				if (strcmp(fontlist[i].f_face, f->f_face) == 0)
					seen = 1;
			if (seen)
				continue;
		}
		memset(M + lfa, 0, 128);
		PW(lfa, f->f_height);
		PW(lfa + 2, f->f_avgw);
		PW(lfa + 8, f->f_weight);
		PB(lfa + 17, (f->f_pitch ? 1 : 2) | f->f_family);
		strcpy((char *)M + lfa + 18, f->f_face);
		memset(M + tma, 0, 64);
		PW(tma, f->f_height);
		PW(tma + 2, f->f_ascent);
		PW(tma + 4, f->f_descent);
		PW(tma + 10, f->f_avgw);
		PW(tma + 12, f->f_maxw);
		PW(tma + 14, f->f_weight);
		PB(tma + 19, f->f_first);
		PB(tma + 20, f->f_last);
		PB(tma + 23, (f->f_pitch ? 0 : 1) | f->f_family);
		PW(tma + 27, 96);
		PW(tma + 29, 96);
		dsave = 0;
		(void)dsave;
		cb_begin();
		cb_push32(FP(lf, 0));
		cb_push32(FP(tm, 0));
		cb_push16(1);		/* RASTER_FONTTYPE */
		cb_push32(a[3]);
		r = cb_call(a[2], 0) & 0xffff;
	}
	g_free(lf);
	g_free(tm);
	(void)fam;
	return r;
}

static u32 g_EnumFonts(a) u32 *a; { return enumfonts(a, 0); }
static u32 g_EnumFontFamilies(a) u32 *a; { return enumfonts(a, 1); }
/* AddFontResource(file): its fonts join the list (a module handle in the low word is not handled) */
static u32
g_AddFontResource(a)
	u32 *a;
{
	extern int font_add();

	return FPSEL(a[0]) ? font_add(STR(a[0])) : 0;
}
static u32 g_SetObjectOwner(a) u32 *a; { return 1; }
static u32 g_RemoveFontResource(a) u32 *a; { return 0; }

/* ---- regions ---- */

static u16
mkrgn()
{
	u16 h = gobj_new(OBJ_RGN);

	rgn_init(&gobj(h, 0)->u.rgn);
	return h;
}

static u32
g_CreateRectRgn(a)
	u32 *a;
{
	u16 h = mkrgn();
	struct rect r;

	r_set(&r, (short)a[0], (short)a[1], (short)a[2], (short)a[3]);
	rgn_set(&gobj(h, 0)->u.rgn, &r);
	return h;
}

static u32
g_CreateRectRgnIndirect(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));
	u16 h = mkrgn();
	struct rect r;

	if (p) {
		r_get(&r, p);
		rgn_set(&gobj(h, 0)->u.rgn, &r);
	}
	return h;
}

static void
ellrgn(g, r)
	struct rgn *g;
	struct rect *r;
{
	int y;
	double a = (r->r - r->l) / 2.0, b = (r->b - r->t) / 2.0, cy = (r->t + r->b) / 2.0, t, v;
	struct rect s;

	g->n = 0;
	for (y = r->t; y < r->b; y++) {
		t = (y + 0.5 - cy) / b;
		v = 1 - t * t;
		if (v <= 0)
			continue;
		v = a * sqrt(v);
		r_set(&s, (int)((r->l + r->r) / 2.0 - v + 0.5), y, (int)((r->l + r->r) / 2.0 + v + 0.5), y + 1);
		rgn_addrect(g, &s);
	}
}

static u32
g_CreateEllipticRgn(a)
	u32 *a;
{
	u16 h = mkrgn();
	struct rect r;

	r_set(&r, (short)a[0], (short)a[1], (short)a[2], (short)a[3]);
	ellrgn(&gobj(h, 0)->u.rgn, &r);
	return h;
}

static u32
g_CreateEllipticRgnIndirect(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));
	u16 h = mkrgn();
	struct rect r;

	if (p) {
		r_get(&r, p);
		ellrgn(&gobj(h, 0)->u.rgn, &r);
	}
	return h;
}

static u32
g_CreateRoundRectRgn(a)
	u32 *a;
{
	return g_CreateRectRgn(a);
}

static u32
g_CreatePolygonRgn(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));
	int n = (short)a[1], i, y, miny, maxy, *xs, nx, j, k, t;
	u16 h = mkrgn();
	struct rgn *g = &gobj(h, 0)->u.rgn;
	struct rect r;

	if (!p || n < 3)
		return h;
	miny = maxy = (short)GW(p + 2);
	for (i = 1; i < n; i++) {
		y = (short)GW(p + 4 * i + 2);
		if (y < miny) miny = y;
		if (y > maxy) maxy = y;
	}
	xs = (int *)malloc(sizeof(int) * (n + 1));
	for (y = miny; y < maxy; y++) {
		double fy = y + 0.5;

		nx = 0;
		for (i = 0; i < n; i++) {
			int ax = (short)GW(p + 4 * i), ay = (short)GW(p + 4 * i + 2);
			int bx = (short)GW(p + 4 * ((i + 1) % n)), by = (short)GW(p + 4 * ((i + 1) % n) + 2);

			if (ay == by || (fy < ay && fy < by) || (fy >= ay && fy >= by))
				continue;
			xs[nx++] = (int)(ax + (fy - ay) * (bx - ax) / (double)(by - ay) + 0.5);
		}
		for (j = 1; j < nx; j++)
			for (k = j; k > 0 && xs[k - 1] > xs[k]; k--)
				t = xs[k], xs[k] = xs[k - 1], xs[k - 1] = t;
		for (j = 0; j + 1 < nx; j += 2) {
			r_set(&r, xs[j], y, xs[j + 1], y + 1);
			rgn_addrect(g, &r);
		}
	}
	free(xs);
	return h;
}

#define	RGN_AND		1
#define	RGN_OR		2
#define	RGN_XOR		3
#define	RGN_DIFF	4
#define	RGN_COPY	5

static u32
g_CombineRgn(a)
	u32 *a;
{
	struct gobj *d = gobj(a[0], OBJ_RGN), *x = gobj(a[1], OBJ_RGN), *y = gobj(a[2], OBJ_RGN);

	if (!d || !x || (a[3] != RGN_COPY && !y))
		return 0;
	switch (a[3]) {
	case RGN_AND: rgn_and(&d->u.rgn, &x->u.rgn, &y->u.rgn); break;
	case RGN_OR: rgn_or(&d->u.rgn, &x->u.rgn, &y->u.rgn); break;
	case RGN_XOR: rgn_xor(&d->u.rgn, &x->u.rgn, &y->u.rgn); break;
	case RGN_DIFF: rgn_diff(&d->u.rgn, &x->u.rgn, &y->u.rgn); break;
	case RGN_COPY: rgn_copy(&d->u.rgn, &x->u.rgn); break;
	default: return 0;
	}
	return rgn_kind(&d->u.rgn);
}

static u32
g_OffsetRgn(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_RGN);

	if (!o)
		return 0;
	rgn_offset(&o->u.rgn, (short)a[1], (short)a[2]);
	return rgn_kind(&o->u.rgn);
}

static u32
g_GetRgnBox(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_RGN);
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));

	if (!o)
		return 0;
	if (p)
		r_put(p, &o->u.rgn.box);
	return rgn_kind(&o->u.rgn);
}

static u32
g_SetRectRgn(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_RGN);
	struct rect r;

	if (o) {
		r_set(&r, (short)a[1], (short)a[2], (short)a[3], (short)a[4]);
		rgn_set(&o->u.rgn, &r);
	}
	return 0;
}

static u32
g_PtInRegion(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_RGN);

	return o && rgn_ptin(&o->u.rgn, (short)a[1], (short)a[2]);
}

static u32
g_RectInRegion(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_RGN);
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	struct rect r;

	if (!o || !p)
		return 0;
	r_get(&r, p);
	return rgn_rectin(&o->u.rgn, &r);
}

static u32
g_EqualRgn(a)
	u32 *a;
{
	struct gobj *x = gobj(a[0], OBJ_RGN), *y = gobj(a[1], OBJ_RGN);

	return x && y && rgn_equal(&x->u.rgn, &y->u.rgn);
}

/* a region (logical, MM_TEXT assumed) drawn with a brush */
static u32
rgnfill(hdc, hr, hb, rop)
	u32 hdc, hr, hb;
	int rop;
{
	struct gobj *o = gobj(hr, OBJ_RGN);
	struct rect r;
	int i;
	struct dc *dc = dc_get(hdc);

	if (!dc || !o)
		return 0;
	for (i = 0; i < o->u.rgn.n; i++) {
		rtosurf(dc, &r, o->u.rgn.r[i].l, o->u.rgn.r[i].t, o->u.rgn.r[i].r, o->u.rgn.r[i].b);
		d_fill(dc, &r, hb, rop);
	}
	return 1;
}

static u32 g_FillRgn(a) u32 *a; { return rgnfill(a[0], a[1], a[2], 0xf0); }
static u32 g_PaintRgn(a) u32 *a; { struct dc *dc = dc_get(a[0]); return dc ? rgnfill(a[0], a[1], dc->st.brush, d_fillrop(dc)) : 0; }
static u32 g_InvertRgn(a) u32 *a; { return rgnfill(a[0], a[1], 0, 0x55); }

static u32
g_FrameRgn(a)
	u32 *a;
{
	struct gobj *o = gobj(a[1], OBJ_RGN);
	struct rect r, e;
	int i, w = (short)a[3], h = (short)a[4];
	DC(a[0]);

	if (!o)
		return 0;
	for (i = 0; i < o->u.rgn.n; i++) {
		rtosurf(dc, &r, o->u.rgn.r[i].l, o->u.rgn.r[i].t, o->u.rgn.r[i].r, o->u.rgn.r[i].b);
		r_set(&e, r.l, r.t, r.r, r.t + h); d_fill(dc, &e, a[2], 0xf0);
		r_set(&e, r.l, r.b - h, r.r, r.b); d_fill(dc, &e, a[2], 0xf0);
		r_set(&e, r.l, r.t, r.l + w, r.b); d_fill(dc, &e, a[2], 0xf0);
		r_set(&e, r.r - w, r.t, r.r, r.b); d_fill(dc, &e, a[2], 0xf0);
	}
	return 1;
}

/* ---- clipping ---- */

static struct rgn *
ensureclip(dc)
	struct dc *dc;
{
	struct rect r;

	if (!dc->st.clip) {
		dc->st.clip = (struct rgn *)malloc(sizeof(struct rgn));
		rgn_init(dc->st.clip);
		/* no clip region means all of it: the visible region, from the origin */
		rgn_copy(dc->st.clip, &dc->vis);
		rgn_offset(dc->st.clip, -dc->ox, -dc->oy);
		if (dc->vis.n == 0) {
			r_set(&r, -32768, -32768, 32767, 32767);
			rgn_set(dc->st.clip, &r);
		}
	}
	dc->effok = 0;
	return dc->st.clip;
}

static u32
g_SelectClipRgn(a)
	u32 *a;
{
	struct gobj *o = gobj(a[1], OBJ_RGN);
	DC(a[0]);

	if (!o) {
		if (dc->st.clip) {
			rgn_free(dc->st.clip);
			free(dc->st.clip);
			dc->st.clip = 0;
		}
		dc->effok = 0;
		return SIMPLEREGION;
	}
	ensureclip(dc);
	rgn_copy(dc->st.clip, &o->u.rgn);
	dc->effok = 0;
	return rgn_kind(dc_clip(dc));
}

/* a logical rectangle in device units from the DC origin */
static void
rdev(dc, r, l, t, rr, b)
	struct dc *dc;
	struct rect *r;
	int l, t, rr, b;
{
	rtosurf(dc, r, l, t, rr, b);
	r->l -= dc->ox;
	r->r -= dc->ox;
	r->t -= dc->oy;
	r->b -= dc->oy;
}

static u32
g_IntersectClipRect(a)
	u32 *a;
{
	struct rect r;
	DC(a[0]);

	rdev(dc, &r, (short)a[1], (short)a[2], (short)a[3], (short)a[4]);
	rgn_andrect(ensureclip(dc), &r);
	return rgn_kind(dc_clip(dc));
}

static u32
g_ExcludeClipRect(a)
	u32 *a;
{
	struct rect r;
	DC(a[0]);

	rdev(dc, &r, (short)a[1], (short)a[2], (short)a[3], (short)a[4]);
	rgn_subrect(ensureclip(dc), &r);
	return rgn_kind(dc_clip(dc));
}

static u32
g_OffsetClipRgn(a)
	u32 *a;
{
	DC(a[0]);

	rgn_offset(ensureclip(dc), lx2dx(dc, (short)a[1]), ly2dy(dc, (short)a[2]));
	return rgn_kind(dc_clip(dc));
}

static u32
g_GetClipBox(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	struct rgn *g;
	struct rect r;
	int l, t, rr, b;
	DC(a[0]);

	g = dc_clip(dc);
	l = g->box.l - dc->ox;
	t = g->box.t - dc->oy;
	rr = g->box.r - dc->ox;
	b = g->box.b - dc->oy;
	dp2lp(dc, &l, &t);
	dp2lp(dc, &rr, &b);
	r_set(&r, l < rr ? l : rr, t < b ? t : b, l < rr ? rr : l, t < b ? b : t);
	if (p)
		r_put(p, &r);
	return g->n ? rgn_kind(g) : NULLREGION;
}

static u32
g_RectVisible(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	struct rect lr, r;
	DC(a[0]);

	if (!p)
		return 0;
	r_get(&lr, p);
	rtosurf(dc, &r, lr.l, lr.t, lr.r, lr.b);
	return rgn_rectin(dc_clip(dc), &r);
}

static u32
g_PtVisible(a)
	u32 *a;
{
	int x = (short)a[1], y = (short)a[2];
	DC(a[0]);

	tosurf(dc, &x, &y);
	return rgn_ptin(dc_clip(dc), x, y);
}

/* ---- bitmaps ---- */

static u32
g_CreateCompatibleBitmap(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);
	int mono = dc && dc->kind == DCK_MEMORY && dc->s && dc->s->mono;
	u16 h = gobj_new(OBJ_BITMAP);

	gobj(h, 0)->u.bm = bm_new((short)a[1], (short)a[2], mono);
	return h;
}

static u32 g_CreateDiscardableBitmap(a) u32 *a; { return g_CreateCompatibleBitmap(a); }

/* device bits: mono rows of 16-bit words, colour rows of bytes padded to even */
static void
setbits(b, p, n, bpp)
	struct bitmap *b;
	u32 p, n;
	int bpp;
{
	int x, y, wb;
	u32 i;

	if (b->s.mono || bpp == 1) {
		wb = ((b->s.w + 15) / 16) * 2;
		for (y = 0; y < b->s.h; y++)
			for (x = 0; x < b->s.w; x++) {
				i = y * wb + x / 8;
				if (i >= n)
					return;
				b->s.pix[y * b->s.rowb + x] = b->s.mono ? (M[p + i] >> (7 - x % 8)) & 1 :
				    ((M[p + i] >> (7 - x % 8)) & 1 ? 255 : 0);
			}
		return;
	}
	if (bpp == 4) {
		static u8 vga[16] = { 0, 4, 2, 6, 1, 5, 3, 248, 7, 252, 250, 254, 249, 253, 251, 255 };

		wb = ((b->s.w * 4 + 15) / 16) * 2;
		for (y = 0; y < b->s.h; y++)
			for (x = 0; x < b->s.w; x++) {
				i = y * wb + x / 2;
				if (i >= n)
					return;
				b->s.pix[y * b->s.rowb + x] = vga[(M[p + i] >> (x & 1 ? 0 : 4)) & 15];
			}
		return;
	}
	if (bpp == 24) {
		wb = ((b->s.w * 3 + 1) & ~1);
		for (y = 0; y < b->s.h; y++)
			for (x = 0; x < b->s.w; x++) {
				i = y * wb + x * 3;
				if (i + 2 >= n)
					return;
				b->s.pix[y * b->s.rowb + x] = pal_index(RGB(M[p + i + 2], M[p + i + 1], M[p + i]));
			}
		return;
	}
	wb = (b->s.w + 1) & ~1;
	for (y = 0; y < b->s.h; y++) {
		i = y * wb;
		if (i >= n)
			return;
		memcpy(b->s.pix + y * b->s.rowb, M + p + i, n - i < (u32)b->s.w ? n - i : (u32)b->s.w);
	}
}

static u32
getbits(b, p, n)
	struct bitmap *b;
	u32 p, n;
{
	int x, y, wb;
	u32 i, t = 0;

	if (b->s.mono) {
		wb = ((b->s.w + 15) / 16) * 2;
		for (y = 0; y < b->s.h; y++)
			for (i = 0; i < (u32)wb; i++) {
				u8 v = 0;

				for (x = 0; x < 8; x++)
					if ((int)(i * 8 + x) < b->s.w && b->s.pix[y * b->s.rowb + i * 8 + x])
						v |= 0x80 >> x;
				if (t >= n)
					return t;
				M[p + t++] = v;
			}
		return t;
	}
	wb = (b->s.w + 1) & ~1;
	for (y = 0; y < b->s.h; y++)
		for (x = 0; x < wb; x++) {
			if (t >= n)
				return t;
			M[p + t++] = x < b->s.w ? b->s.pix[y * b->s.rowb + x] : 0;
		}
	return t;
}

static u32
g_CreateBitmap(a)
	u32 *a;
{
	int w = (short)a[0], h = (short)a[1], bpp = a[2] * a[3];
	u32 p = a[4] ? lin(FPSEL(a[4]), FPOFF(a[4])) : 0;
	u16 hb = gobj_new(OBJ_BITMAP);
	struct bitmap *b;

	b = bm_new(w, h, bpp == 1);
	gobj(hb, 0)->u.bm = b;
	if (p)
		setbits(b, p, MSIZE - p, bpp);
	return hb;
}

static u32
g_CreateBitmapIndirect(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0])), b[5];

	if (!p)
		return 0;
	b[0] = GW(p + 2);
	b[1] = GW(p + 4);
	b[2] = M[p + 8];
	b[3] = M[p + 9];
	b[4] = GL(p + 10);
	return g_CreateBitmap(b);
}

static u32
g_SetBitmapBits(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_BITMAP);
	u32 p = lin(FPSEL(a[2]), FPOFF(a[2]));

	if (!o || !p)
		return 0;
	setbits(o->u.bm, p, a[1], o->u.bm->s.mono ? 1 : 8);
	return a[1];
}

static u32
g_GetBitmapBits(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_BITMAP);
	u32 p = lin(FPSEL(a[2]), FPOFF(a[2]));

	if (!o || !p)
		return 0;
	return getbits(o->u.bm, p, a[1]);
}

static u32
g_SetBitmapDimension(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_BITMAP);
	u32 r;

	if (!o)
		return 0;
	r = FP(o->u.bm->dimy, o->u.bm->dimx);
	o->u.bm->dimx = (short)a[1];
	o->u.bm->dimy = (short)a[2];
	return r;
}

static u32
g_GetBitmapDimension(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_BITMAP);

	return o ? FP(o->u.bm->dimy, o->u.bm->dimx) : 0;
}

/* ---- DIBs ---- */

/*
 * A DIB (header at hdr, bits at bits) as a new bitmap; usage 1 is
 * DIB_PAL_COLORS (the colour table indexes the DC's palette).
 * A two-colour black and white DIB gives a monochrome bitmap when mono.
 */
struct bitmap *
dib_bitmap(hdr, bits, usage, mono, start, lines)
	u32 hdr, bits;
	int usage, mono, start, lines;
{
	int core = GL(hdr) == 12, w, h, bpp, ncol, x, y, comp, stride, i, flip = 1;
	u8 map[256];
	u32 ct, row;
	struct bitmap *b;

	if (core) {
		w = GW(hdr + 4);
		h = (short)GW(hdr + 6);
		bpp = GW(hdr + 10);
		comp = 0;
		ncol = bpp <= 8 ? 1 << bpp : 0;
		ct = hdr + 12;
	} else {
		w = GL(hdr + 4);
		h = (s32)GL(hdr + 8);
		bpp = GW(hdr + 14);
		comp = GL(hdr + 16);
		ncol = GL(hdr + 32);
		if (!ncol && bpp <= 8)
			ncol = 1 << bpp;
		ct = hdr + GL(hdr);
	}
	if (h < 0) {
		h = -h;
		flip = 0;
	}
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
		return 0;
	for (i = 0; i < ncol && i < 256; i++) {
		if (usage == 1)
			map[i] = GW(ct + 2 * i) & 0xff;
		else if (core)
			map[i] = pal_index(RGB(M[ct + 3 * i + 2], M[ct + 3 * i + 1], M[ct + 3 * i]));
		else
			map[i] = pal_index(RGB(M[ct + 4 * i + 2], M[ct + 4 * i + 1], M[ct + 4 * i]));
	}
	if (!bits)
		bits = ct + (usage == 1 ? 2 : core ? 3 : 4) * ncol;
	mono = mono && bpp == 1 && ((map[0] == 0 && map[1] == 255) || (map[0] == 255 && map[1] == 0));
	b = bm_new(w, h, mono);
	stride = ((w * bpp + 31) / 32) * 4;
	if (comp == 1 || comp == 2) {
		/* RLE8 and RLE4 */
		u32 p = bits;
		int px = 0, py = 0, c, d, k;

		for (;;) {
			c = M[p++];
			d = M[p++];
			if (c) {
				for (k = 0; k < c; k++, px++)
					if (px < w && py < h)
						b->s.pix[(flip ? h - 1 - py : py) * w + px] = comp == 1 ? map[d] :
						    map[(k & 1) ? d & 15 : d >> 4];
				continue;
			}
			if (d == 0) {
				px = 0;
				py++;
			} else if (d == 1)
				break;
			else if (d == 2) {
				px += M[p++];
				py += M[p++];
			} else {
				for (k = 0; k < d; k++, px++) {
					int v = comp == 1 ? M[p + k] : (M[p + k / 2] >> ((k & 1) ? 0 : 4)) & 15;

					if (px < w && py < h)
						b->s.pix[(flip ? h - 1 - py : py) * w + px] = map[v];
				}
				p += comp == 1 ? (d + 1) & ~1 : (((d + 1) / 2) + 1) & ~1;
			}
			if (py >= h || p >= MSIZE)
				break;
		}
		return b;
	}
	for (y = 0; y < h; y++) {
		int dy = flip ? h - 1 - y : y;

		if (lines && (y < start || y >= start + lines))
			continue;
		row = bits + (u32)(y - (lines ? start : 0)) * stride;
		if (row + stride > MSIZE)
			break;
		for (x = 0; x < w; x++) {
			int v;

			switch (bpp) {
			case 1: v = map[(M[row + x / 8] >> (7 - x % 8)) & 1]; break;
			case 4: v = map[(M[row + x / 2] >> (x & 1 ? 0 : 4)) & 15]; break;
			case 8: v = map[M[row + x]]; break;
			case 24: v = pal_index(RGB(M[row + 3 * x + 2], M[row + 3 * x + 1], M[row + 3 * x])); break;
			default: v = 0;
			}
			if (mono)
				v = syspal[v][0] + syspal[v][1] + syspal[v][2] >= 384;
			b->s.pix[dy * w + x] = v;
		}
	}
	return b;
}

u16
bitmap_handle(b)
	struct bitmap *b;
{
	u16 h;

	if (!b)
		return 0;
	h = gobj_new(OBJ_BITMAP);
	gobj(h, 0)->u.bm = b;
	return h;
}

static u32
g_CreateDIBitmap(a)
	u32 *a;
{
	u32 hdr = lin(FPSEL(a[1]), FPOFF(a[1])), bi = lin(FPSEL(a[4]), FPOFF(a[4]));
	u32 bits = lin(FPSEL(a[3]), FPOFF(a[3]));
	struct bitmap *b;

	if (!hdr)
		return 0;
	if (!(a[2] & 4) || !bits || !bi) {	/* not CBM_INIT: just the size */
		int w = GL(hdr) == 12 ? GW(hdr + 4) : GL(hdr + 4);
		int h = GL(hdr) == 12 ? GW(hdr + 6) : GL(hdr + 8);

		return bitmap_handle(bm_new(w, h < 0 ? -h : h, 0));
	}
	b = dib_bitmap(bi, bits, a[5], 0, 0, 0);
	return bitmap_handle(b);
}

static u32
dibdraw(dc, dx, dy, dw, dh, sx, sy, sw, sh, bits, bi, usage, rop, start, lines)
	struct dc *dc;
	int dx, dy, dw, dh, sx, sy, sw, sh, usage, start, lines;
	u32 bits, bi, rop;
{
	struct bitmap *b = dib_bitmap(bi, bits, usage, 0, start, lines);
	struct dc tmp;
	int x2 = dx + dw, y2 = dy + dh, h;

	if (!b)
		return 0;
	memset(&tmp, 0, sizeof tmp);
	tmp.s = &b->s;
	h = b->s.h;
	/* DIB source y counts from the bottom */
	sy = h - sy - sh;
	tosurf(dc, &dx, &dy);
	tosurf(dc, &x2, &y2);
	d_stretch(dc, dx, dy, x2 - dx, y2 - dy, &tmp, sx, sy, sw, sh, rop);
	bm_free(b);
	return sh;
}

static u32
g_StretchDIBits(a)
	u32 *a;
{
	u32 bits = lin(FPSEL(a[9]), FPOFF(a[9])), bi = lin(FPSEL(a[10]), FPOFF(a[10]));
	DC(a[0]);

	if (!bits || !bi)
		return 0;
	return dibdraw(dc, (short)a[1], (short)a[2], (short)a[3], (short)a[4], (short)a[5], (short)a[6],
	    (short)a[7], (short)a[8], bits, bi, a[11], a[12], 0, 0);
}

static u32
g_SetDIBitsToDevice(a)
	u32 *a;
{
	u32 bits = lin(FPSEL(a[9]), FPOFF(a[9])), bi = lin(FPSEL(a[10]), FPOFF(a[10]));
	int w = (short)a[3], h = (short)a[4];
	DC(a[0]);

	if (!bits || !bi)
		return 0;
	return dibdraw(dc, (short)a[1], (short)a[2], w, h, (short)a[5], (short)a[6], w, h,
	    bits, bi, a[11], SRCCOPY, a[7], a[8]);
}

static u32
g_SetDIBits(a)
	u32 *a;
{
	struct gobj *o = gobj(a[1], OBJ_BITMAP);
	u32 bits = lin(FPSEL(a[4]), FPOFF(a[4])), bi = lin(FPSEL(a[5]), FPOFF(a[5]));
	struct bitmap *b;
	int y;

	if (!o || !bits || !bi)
		return 0;
	b = dib_bitmap(bi, bits, a[6], o->u.bm->s.mono, a[2], a[3]);
	if (!b)
		return 0;
	for (y = 0; y < b->s.h && y < o->u.bm->s.h; y++)
		memcpy(o->u.bm->s.pix + y * o->u.bm->s.rowb, b->s.pix + y * b->s.rowb,
		    b->s.w < o->u.bm->s.w ? b->s.w : o->u.bm->s.w);
	bm_free(b);
	return a[3];
}

/* GetDIBits: 1, 4, 8 or 24 bits with our palette as the colour table */
static u32
g_GetDIBits(a)
	u32 *a;
{
	struct gobj *o = gobj(a[1], OBJ_BITMAP);
	u32 bits = lin(FPSEL(a[4]), FPOFF(a[4])), bi = lin(FPSEL(a[5]), FPOFF(a[5]));
	struct bitmap *b;
	int bpp, x, y, stride, start = a[2], n = a[3], i, ncol;
	u32 row;

	if (!o || !bi)
		return 0;
	b = o->u.bm;
	if (GW(bi + 14) == 0) {		/* fill in the header */
		PL(bi + 4, b->s.w);
		PL(bi + 8, b->s.h);
		PW(bi + 12, 1);
		PW(bi + 14, b->s.mono ? 1 : 8);
		PL(bi + 16, 0);
		PL(bi + 20, ((b->s.w * (b->s.mono ? 1 : 8) + 31) / 32) * 4 * b->s.h);
		return b->s.h;
	}
	bpp = GW(bi + 14);
	ncol = bpp <= 8 ? 1 << bpp : 0;
	for (i = 0; i < ncol; i++) {
		int s = bpp == 1 ? (i ? 255 : 0) : bpp == 4 ? (i < 8 ? i : 248 + i - 8) : i;

		PB(bi + 40 + 4 * i, syspal[s][2]);
		PB(bi + 40 + 4 * i + 1, syspal[s][1]);
		PB(bi + 40 + 4 * i + 2, syspal[s][0]);
		PB(bi + 40 + 4 * i + 3, 0);
	}
	if (!bits)
		return b->s.h;
	stride = ((b->s.w * bpp + 31) / 32) * 4;
	for (y = start; y < start + n && y < b->s.h; y++) {
		u8 *src = b->s.pix + (b->s.h - 1 - y) * b->s.rowb;

		row = bits + (u32)(y - start) * stride;
		if (row + stride > MSIZE)
			break;
		memset(M + row, 0, stride);
		for (x = 0; x < b->s.w; x++) {
			int v = src[x];

			switch (bpp) {
			case 1:
				if (b->s.mono ? v : syspal[v][0] + syspal[v][1] + syspal[v][2] >= 384)
					M[row + x / 8] |= 0x80 >> (x % 8);
				break;
			case 4:
				v = b->s.mono ? (v ? 15 : 0) : pal4(v);
				M[row + x / 2] |= x & 1 ? v : v << 4;
				break;
			case 8:
				M[row + x] = b->s.mono ? (v ? 255 : 0) : v;
				break;
			case 24:
				if (b->s.mono)
					v = v ? 255 : 0;
				M[row + 3 * x] = syspal[v][2];
				M[row + 3 * x + 1] = syspal[v][1];
				M[row + 3 * x + 2] = syspal[v][0];
				break;
			}
		}
	}
	return y - start;
}

/* the nearest of the 16 VGA colours as a 4-bit DIB index */
int
pal4(v)
	int v;
{
	int i, best = 0, bd = 1 << 30, d, s;

	for (i = 0; i < 16; i++) {
		s = i < 8 ? i : 248 + i - 8;
		d = (syspal[s][0] - syspal[v][0]) * (syspal[s][0] - syspal[v][0]) +
		    (syspal[s][1] - syspal[v][1]) * (syspal[s][1] - syspal[v][1]) +
		    (syspal[s][2] - syspal[v][2]) * (syspal[s][2] - syspal[v][2]);
		if (d < bd)
			bd = d, best = i;
	}
	return best;
}

static u32
dib_brush(hg, usage)
	u32 hg;
	int usage;
{
	u32 p = lin(hg, 0);
	struct bitmap *b;
	u16 h, hb;

	if (!p)
		return 0;
	b = dib_bitmap(p, 0, usage, 0, 0, 0);
	if (!b)
		return 0;
	hb = bitmap_handle(b);
	h = patbrush(hb);
	gobj(h, 0)->u.brush.bitmap = 0;
	gobj_delete(hb);
	return h;
}

/* ---- palettes: logical palettes map to the fixed system palette ---- */

static u32
g_CreatePalette(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));
	u16 h;
	struct palette *pal;
	int i;

	if (!p)
		return 0;
	h = gobj_new(OBJ_PAL);
	pal = (struct palette *)calloc(1, sizeof *pal);
	gobj(h, 0)->u.pal = pal;
	pal->n = GW(p + 2) > 256 ? 256 : GW(p + 2);
	for (i = 0; i < pal->n; i++) {
		memcpy(pal->ent[i], M + p + 4 + 4 * i, 4);
		pal->map[i] = pal_index(RGB(pal->ent[i][0], pal->ent[i][1], pal->ent[i][2]));
	}
	return h;
}

static u32
g_SelectPalette(a)
	u32 *a;
{
	u16 o;
	DC(a[0]);

	if (!gobj(a[1], OBJ_PAL))
		return 0;
	o = dc->st.pal;
	dc->st.pal = a[1];
	return o;
}

static u32 g_RealizePalette(a) u32 *a; { return 0; }
static u32 g_UpdateColors(a) u32 *a; { return 0; }

static u32
g_GetPaletteEntries(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_PAL);
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3]));
	int i;

	if (!o)
		return 0;
	for (i = 0; i < (int)a[2] && (int)a[1] + i < o->u.pal->n; i++)
		if (p)
			memcpy(M + p + 4 * i, o->u.pal->ent[a[1] + i], 4);
	return i;
}

static u32
g_SetPaletteEntries(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_PAL);
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3]));
	int i, k;

	if (!o || !p)
		return 0;
	for (i = 0; i < (int)a[2] && (int)a[1] + i < o->u.pal->n; i++) {
		k = a[1] + i;
		memcpy(o->u.pal->ent[k], M + p + 4 * i, 4);
		o->u.pal->map[k] = pal_index(RGB(o->u.pal->ent[k][0], o->u.pal->ent[k][1], o->u.pal->ent[k][2]));
	}
	return i;
}

static u32
g_ResizePalette(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], OBJ_PAL);

	if (!o || a[1] > 256)
		return 0;
	o->u.pal->n = a[1];
	return 1;
}

static u32
g_GetSystemPaletteEntries(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3]));
	int i;

	for (i = 0; p && i < (int)a[2] && (int)a[1] + i < 256; i++) {
		memcpy(M + p + 4 * i, syspal[a[1] + i], 3);
		M[p + 4 * i + 3] = 0;
	}
	return i;
}

static u32 g_GetSystemPaletteUse(a) u32 *a; { return 1; }
static u32 g_GetNearestPaletteIndex(a) u32 *a; { return pal_index(a[1]); }
static u32 g_AnimatePalette(a) u32 *a; { return 1; }

/* ---- the rest ---- */

static u32 g_Escape(a) u32 *a; { return 0; }
static u32 g_zero(a) u32 *a; { return 0; }
static u32 g_one(a) u32 *a; { return 1; }

static u32
g_IsGDIObject(a)
	u32 *a;
{
	struct gobj *o = gobj(a[0], 0);

	return o ? o->type : 0;
}

/* metafiles: a DC that records nothing; playing one draws nothing */
static u32
g_CreateMetaFile(a)
	u32 *a;
{
	u16 h = dc_new(DCK_INFO);

	dc_get(h)->s = &screen;
	return h;
}

static u32
g_CloseMetaFile(a)
	u32 *a;
{
	u16 h;

	dc_free(a[0]);
	h = g_alloc(GMEM_ZEROINIT, 18, 0);
	PW(sel_base(h), 1);
	PW(sel_base(h) + 2, 9);
	PW(sel_base(h) + 4, 0x300);
	PL(sel_base(h) + 6, 9);
	return h;
}

static u32 g_DeleteMetaFile(a) u32 *a; { g_free(a[0]); return 1; }

struct impl g_impl[] = {
	{ "GDI", "CreateCompatibleDC", g_CreateCompatibleDC },
	{ "GDI", "CreateDC", g_CreateDC },
	{ "GDI", "CreateIC", g_CreateDC },
	{ "GDI", "DeleteDC", g_DeleteDC },
	{ "GDI", "SaveDC", g_SaveDC },
	{ "GDI", "RestoreDC", g_RestoreDC },
	{ "GDI", "GetDeviceCaps", g_GetDeviceCaps },
	{ "GDI", "SelectObject", g_SelectObject },
	{ "GDI", "GetStockObject", g_GetStockObject },
	{ "GDI", "DeleteObject", g_DeleteObject },
	{ "GDI", "UnrealizeObject", g_UnrealizeObject },
	{ "GDI", "GetObject", g_GetObject },
	{ "GDI", "IsGDIObject", g_IsGDIObject },
	{ "GDI", "CreatePen", g_CreatePen },
	{ "GDI", "CreatePenIndirect", g_CreatePenIndirect },
	{ "GDI", "CreateSolidBrush", g_CreateSolidBrush },
	{ "GDI", "CreateHatchBrush", g_CreateHatchBrush },
	{ "GDI", "CreatePatternBrush", g_CreatePatternBrush },
	{ "GDI", "CreateBrushIndirect", g_CreateBrushIndirect },
	{ "GDI", "CreateDIBPatternBrush", g_CreateDIBPatternBrush },
	{ "GDI", "CreateFont", g_CreateFont },
	{ "GDI", "CreateFontIndirect", g_CreateFontIndirect },
	{ "GDI", "SetTextColor", g_SetTextColor },
	{ "GDI", "GetTextColor", g_GetTextColor },
	{ "GDI", "SetBkColor", g_SetBkColor },
	{ "GDI", "GetBkColor", g_GetBkColor },
	{ "GDI", "SetBkMode", g_SetBkMode },
	{ "GDI", "GetBkMode", g_GetBkMode },
	{ "GDI", "SetROP2", g_SetROP2 },
	{ "GDI", "GetROP2", g_GetROP2 },
	{ "GDI", "SetPolyFillMode", g_SetPolyFillMode },
	{ "GDI", "GetPolyFillMode", g_GetPolyFillMode },
	{ "GDI", "SetStretchBltMode", g_SetStretchBltMode },
	{ "GDI", "GetStretchBltMode", g_GetStretchBltMode },
	{ "GDI", "SetTextAlign", g_SetTextAlign },
	{ "GDI", "GetTextAlign", g_GetTextAlign },
	{ "GDI", "SetTextCharacterExtra", g_SetTextCharacterExtra },
	{ "GDI", "GetTextCharacterExtra", g_GetTextCharacterExtra },
	{ "GDI", "SetTextJustification", g_SetTextJustification },
	{ "GDI", "SetBrushOrg", g_SetBrushOrg },
	{ "GDI", "GetBrushOrg", g_GetBrushOrg },
	{ "GDI", "GetDCOrg", g_GetDCOrg },
	{ "GDI", "GetCurrentPosition", g_GetCurrentPosition },
	{ "GDI", "GetNearestColor", g_GetNearestColor },
	{ "GDI", "SetMapMode", g_SetMapMode },
	{ "GDI", "GetMapMode", g_GetMapMode },
	{ "GDI", "SetWindowOrg", g_SetWindowOrg },
	{ "GDI", "SetViewportOrg", g_SetViewportOrg },
	{ "GDI", "GetWindowOrg", g_GetWindowOrg },
	{ "GDI", "GetViewportOrg", g_GetViewportOrg },
	{ "GDI", "SetWindowExt", g_SetWindowExt },
	{ "GDI", "SetViewportExt", g_SetViewportExt },
	{ "GDI", "GetWindowExt", g_GetWindowExt },
	{ "GDI", "GetViewportExt", g_GetViewportExt },
	{ "GDI", "OffsetWindowOrg", g_OffsetWindowOrg },
	{ "GDI", "OffsetViewportOrg", g_OffsetViewportOrg },
	{ "GDI", "ScaleViewportExt", g_ScaleViewportExt },
	{ "GDI", "ScaleWindowExt", g_ScaleWindowExt },
	{ "GDI", "LPtoDP", g_LPtoDP },
	{ "GDI", "DPtoLP", g_DPtoLP },
	{ "GDI", "MulDiv", g_MulDiv },
	{ "GDI", "MoveTo", g_MoveTo },
	{ "GDI", "LineTo", g_LineTo },
	{ "GDI", "Rectangle", g_Rectangle },
	{ "GDI", "RoundRect", g_RoundRect },
	{ "GDI", "Ellipse", g_Ellipse },
	{ "GDI", "Polygon", g_Polygon },
	{ "GDI", "Polyline", g_Polyline },
	{ "GDI", "PolyPolygon", g_PolyPolygon },
	{ "GDI", "Arc", g_Arc },
	{ "GDI", "Pie", g_Pie },
	{ "GDI", "Chord", g_Chord },
	{ "GDI", "PatBlt", g_PatBlt },
	{ "GDI", "BitBlt", g_BitBlt },
	{ "GDI", "StretchBlt", g_StretchBlt },
	{ "GDI", "SetPixel", g_SetPixel },
	{ "GDI", "GetPixel", g_GetPixel },
	{ "GDI", "FloodFill", g_FloodFill },
	{ "GDI", "ExtFloodFill", g_ExtFloodFill },
	{ "GDI", "TextOut", g_TextOut },
	{ "GDI", "ExtTextOut", g_ExtTextOut },
	{ "GDI", "GetTextExtent", g_GetTextExtent },
	{ "GDI", "GetTextExtentPoint", g_GetTextExtentPoint },
	{ "GDI", "GetTextMetrics", g_GetTextMetrics },
	{ "GDI", "GetTextFace", g_GetTextFace },
	{ "GDI", "GetCharWidth", g_GetCharWidth },
	{ "GDI", "EnumFonts", g_EnumFonts },
	{ "GDI", "EnumFontFamilies", g_EnumFontFamilies },
	{ "GDI", "AddFontResource", g_AddFontResource },
	{ "GDI", "SetObjectOwner", g_SetObjectOwner },
	{ "GDI", "RemoveFontResource", g_RemoveFontResource },
	{ "GDI", "CreateRectRgn", g_CreateRectRgn },
	{ "GDI", "CreateRectRgnIndirect", g_CreateRectRgnIndirect },
	{ "GDI", "CreateEllipticRgn", g_CreateEllipticRgn },
	{ "GDI", "CreateEllipticRgnIndirect", g_CreateEllipticRgnIndirect },
	{ "GDI", "CreateRoundRectRgn", g_CreateRoundRectRgn },
	{ "GDI", "CreatePolygonRgn", g_CreatePolygonRgn },
	{ "GDI", "CombineRgn", g_CombineRgn },
	{ "GDI", "OffsetRgn", g_OffsetRgn },
	{ "GDI", "GetRgnBox", g_GetRgnBox },
	{ "GDI", "SetRectRgn", g_SetRectRgn },
	{ "GDI", "PtInRegion", g_PtInRegion },
	{ "GDI", "RectInRegion", g_RectInRegion },
	{ "GDI", "EqualRgn", g_EqualRgn },
	{ "GDI", "FillRgn", g_FillRgn },
	{ "GDI", "PaintRgn", g_PaintRgn },
	{ "GDI", "InvertRgn", g_InvertRgn },
	{ "GDI", "FrameRgn", g_FrameRgn },
	{ "GDI", "SelectClipRgn", g_SelectClipRgn },
	{ "GDI", "IntersectClipRect", g_IntersectClipRect },
	{ "GDI", "ExcludeClipRect", g_ExcludeClipRect },
	{ "GDI", "OffsetClipRgn", g_OffsetClipRgn },
	{ "GDI", "GetClipBox", g_GetClipBox },
	{ "GDI", "RectVisible", g_RectVisible },
	{ "GDI", "RectVisibleOld", g_RectVisible },
	{ "GDI", "PtVisible", g_PtVisible },
	{ "GDI", "CreateCompatibleBitmap", g_CreateCompatibleBitmap },
	{ "GDI", "CreateDiscardableBitmap", g_CreateDiscardableBitmap },
	{ "GDI", "CreateBitmap", g_CreateBitmap },
	{ "GDI", "CreateBitmapIndirect", g_CreateBitmapIndirect },
	{ "GDI", "SetBitmapBits", g_SetBitmapBits },
	{ "GDI", "GetBitmapBits", g_GetBitmapBits },
	{ "GDI", "SetBitmapDimension", g_SetBitmapDimension },
	{ "GDI", "GetBitmapDimension", g_GetBitmapDimension },
	{ "GDI", "CreateDIBitmap", g_CreateDIBitmap },
	{ "GDI", "StretchDIBits", g_StretchDIBits },
	{ "GDI", "SetDIBitsToDevice", g_SetDIBitsToDevice },
	{ "GDI", "SetDIBits", g_SetDIBits },
	{ "GDI", "GetDIBits", g_GetDIBits },
	{ "GDI", "CreatePalette", g_CreatePalette },
	{ "GDI", "SelectPalette", g_SelectPalette },
	{ "GDI", "RealizePalette", g_RealizePalette },
	{ "GDI", "UpdateColors", g_UpdateColors },
	{ "GDI", "GetPaletteEntries", g_GetPaletteEntries },
	{ "GDI", "SetPaletteEntries", g_SetPaletteEntries },
	{ "GDI", "ResizePalette", g_ResizePalette },
	{ "GDI", "GetSystemPaletteEntries", g_GetSystemPaletteEntries },
	{ "GDI", "GetSystemPaletteUse", g_GetSystemPaletteUse },
	{ "GDI", "GetNearestPaletteIndex", g_GetNearestPaletteIndex },
	{ "GDI", "AnimatePalette", g_AnimatePalette },
	{ "GDI", "Escape", g_Escape },
	{ "GDI", "CreateMetaFile", g_CreateMetaFile },
	{ "GDI", "CloseMetaFile", g_CloseMetaFile },
	{ "GDI", "DeleteMetaFile", g_DeleteMetaFile },
	{ "GDI", "PlayMetaFile", g_one },
	{ "GDI", "SetMapperFlags", g_zero },
	{ "GDI", "SetEnvironment", g_zero },
	{ "GDI", "GetEnvironment", g_zero },
	{ "GDI", "SetAbortProc", g_one },
	{ "GDI", "StartDoc", g_zero },
	{ "GDI", "EndDoc", g_zero },
	{ "GDI", "StartPage", g_zero },
	{ "GDI", "EndPage", g_zero },
	{ "GDI", "GetRasterizerCaps", g_zero },
	{ "GDI", "GetAspectRatioFilter", g_zero },
	{ 0 }
};

/* logical to surface pixels, for USER */
void
tosurf_pub(dc, x, y)
	struct dc *dc;
	int *x, *y;
{
	tosurf(dc, x, y);
}
