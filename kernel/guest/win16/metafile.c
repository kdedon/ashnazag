/*
 * metafile.c -- GDI's metafiles, in Windows' own format: a METAHEADER
 * (type, header size 9, version 0x300, size in words, objects, largest
 * record, 0) and records (size in words, function, parameters), the
 * META_EOF record last.  A metafile handle is a global block holding
 * them; a disk metafile is read into one.
 *
 * Recording: a metafile DC (CreateMetaFile) is a DC the drawing calls do
 * not draw on: thunk.c hands the GDI calls made on one to meta_record,
 * which writes their records.  Most carry their arguments as they are on
 * the stack (the last first), without the DC; text, points, objects,
 * regions, palettes, bitmaps (as DIBs) and escapes have their own.  An
 * object selected is created in the metafile's table at its first use
 * and named by its slot after; deleting it deletes it there.
 *
 * Playing (PlayMetaFile, EnumMetaFile, PlayMetaFileRecord): each record
 * becomes the call it was, on our GDI, with the metafile's objects in a
 * handle table in guest memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "win.h"
#include "apitab.h"

extern u32 ualloc(), ulin();
extern void ufree();
extern apifn api_fn();
extern u8 syspal[256][3];

#define	META_EOF		0x0000
#define	META_SAVEDC		0x001e
#define	META_REALIZEPALETTE	0x0035
#define	META_SETPALENTRIES	0x0037
#define	META_CREATEPALETTE	0x00f7
#define	META_SETBKMODE		0x0102
#define	META_RESIZEPALETTE	0x0139
#define	META_DIBCREATEPATTERNBRUSH 0x0142
#define	META_INVERTREGION	0x012a
#define	META_PAINTREGION	0x012b
#define	META_SELECTCLIPREGION	0x012c
#define	META_SELECTOBJECT	0x012d
#define	META_DELETEOBJECT	0x01f0
#define	META_CREATEPATTERNBRUSH	0x01f9
#define	META_FILLREGION		0x0228
#define	META_SELECTPALETTE	0x0234
#define	META_CREATEPENINDIRECT	0x02fa
#define	META_CREATEFONTINDIRECT	0x02fb
#define	META_CREATEBRUSHINDIRECT 0x02fc
#define	META_POLYGON		0x0324
#define	META_POLYLINE		0x0325
#define	META_FRAMEREGION	0x0429
#define	META_ANIMATEPALETTE	0x0436
#define	META_TEXTOUT		0x0521
#define	META_POLYPOLYGON	0x0538
#define	META_ESCAPE		0x0626
#define	META_CREATEREGION	0x06ff
#define	META_BITBLT		0x0922
#define	META_DIBBITBLT		0x0940
#define	META_EXTTEXTOUT		0x0a32
#define	META_STRETCHBLT		0x0b23
#define	META_DIBSTRETCHBLT	0x0b41
#define	META_SETDIBTODEV	0x0d33
#define	META_STRETCHDIB		0x0f43

#define	NSLOT	512

/* a metafile being recorded */
struct mf {
	u8	*b;
	long	n, cap;		/* bytes */
	long	maxrec;		/* words */
	u32	slot[NSLOT];	/* the objects in its table, by slot; 0 free */
	int	nslot;		/* slots used at most */
	long	rec;		/* where the record being written starts */
	char	path[300];	/* a disk metafile's DOS name, or "" */
};

/* ---- the calls recorded, and how ---- */

enum { K_PLAIN, K_TEXTOUT, K_EXTTEXTOUT, K_POLY, K_POLYPOLY, K_SELECT, K_SELPAL, K_BITBLT, K_STRETCHBLT,
	K_STRETCHDIB, K_SETDIB, K_ESCAPE, K_FILLRGN, K_FRAMERGN, K_RGN1, K_SELCLIP, K_NONE };

struct metafn {
	char	*name;
	int	code;
	int	kind;
	int	ret;		/* what the call gives back */
	char	*args;		/* its arguments, from apitab */
	apifn	fn;
};

static struct metafn fns[] = {
	{ "SetBkColor", 0x0201, K_PLAIN, 1 },
	{ "SetBkMode", 0x0102, K_PLAIN, 1 },
	{ "SetMapMode", 0x0103, K_PLAIN, 1 },
	{ "SetROP2", 0x0104, K_PLAIN, 1 },
	{ "SetRelAbs", 0x0105, K_PLAIN, 1 },
	{ "SetPolyFillMode", 0x0106, K_PLAIN, 1 },
	{ "SetStretchBltMode", 0x0107, K_PLAIN, 1 },
	{ "SetTextCharacterExtra", 0x0108, K_PLAIN, 1 },
	{ "SetTextColor", 0x0209, K_PLAIN, 1 },
	{ "SetTextJustification", 0x020a, K_PLAIN, 1 },
	{ "SetWindowOrg", 0x020b, K_PLAIN, 1 },
	{ "SetWindowExt", 0x020c, K_PLAIN, 1 },
	{ "SetViewportOrg", 0x020d, K_PLAIN, 1 },
	{ "SetViewportExt", 0x020e, K_PLAIN, 1 },
	{ "OffsetWindowOrg", 0x020f, K_PLAIN, 1 },
	{ "ScaleWindowExt", 0x0410, K_PLAIN, 1 },
	{ "OffsetViewportOrg", 0x0211, K_PLAIN, 1 },
	{ "ScaleViewportExt", 0x0412, K_PLAIN, 1 },
	{ "LineTo", 0x0213, K_PLAIN, 1 },
	{ "MoveTo", 0x0214, K_PLAIN, 1 },
	{ "ExcludeClipRect", 0x0415, K_PLAIN, 2 },
	{ "IntersectClipRect", 0x0416, K_PLAIN, 2 },
	{ "Arc", 0x0817, K_PLAIN, 1 },
	{ "Ellipse", 0x0418, K_PLAIN, 1 },
	{ "FloodFill", 0x0419, K_PLAIN, 1 },
	{ "Pie", 0x081a, K_PLAIN, 1 },
	{ "Rectangle", 0x041b, K_PLAIN, 1 },
	{ "RoundRect", 0x061c, K_PLAIN, 1 },
	{ "PatBlt", 0x061d, K_PLAIN, 1 },
	{ "SaveDC", 0x001e, K_PLAIN, 1 },
	{ "SetPixel", 0x041f, K_PLAIN, 1 },
	{ "OffsetClipRgn", 0x0220, K_PLAIN, 2 },
	{ "TextOut", 0x0521, K_TEXTOUT, 1 },
	{ "BitBlt", 0x0940, K_BITBLT, 1 },
	{ "StretchBlt", 0x0b41, K_STRETCHBLT, 1 },
	{ "Polygon", 0x0324, K_POLY, 1 },
	{ "Polyline", 0x0325, K_POLY, 1 },
	{ "Escape", 0x0626, K_ESCAPE, 1 },
	{ "RestoreDC", 0x0127, K_PLAIN, 1 },
	{ "FillRgn", 0x0228, K_FILLRGN, 1 },
	{ "FrameRgn", 0x0429, K_FRAMERGN, 1 },
	{ "InvertRgn", 0x012a, K_RGN1, 1 },
	{ "PaintRgn", 0x012b, K_RGN1, 1 },
	{ "SelectClipRgn", 0x012c, K_SELCLIP, 2 },
	{ "SelectObject", 0x012d, K_SELECT, 0 },
	{ "SetTextAlign", 0x012e, K_PLAIN, 1 },
	{ "Chord", 0x0830, K_PLAIN, 1 },
	{ "SetMapperFlags", 0x0231, K_PLAIN, 1 },
	{ "ExtTextOut", 0x0a32, K_EXTTEXTOUT, 1 },
	{ "SetDIBitsToDevice", 0x0d33, K_SETDIB, 1 },
	{ "SelectPalette", 0x0234, K_SELPAL, 0 },
	{ "RealizePalette", 0x0035, K_PLAIN, 0 },
	{ "PolyPolygon", 0x0538, K_POLYPOLY, 1 },
	{ "StretchDIBits", 0x0f43, K_STRETCHDIB, 1 },
	{ "ExtFloodFill", 0x0548, K_PLAIN, 1 },
	{ 0 }
};

static int inited;

static void
init()
{
	struct apimod *m;
	struct apient *e;
	struct metafn *f;

	inited = 1;
	for (m = apimods; m->am_name && strcmp(m->am_name, "GDI"); m++)
		;
	for (f = fns; f->name; f++) {
		for (e = m->am_ent; e && e->ae_name; e++)
			if (strcmp(e->ae_name, f->name) == 0)
				f->args = e->ae_args;
		f->fn = api_fn("GDI", f->name);
	}
}

/* the way a GDI call is recorded, 0 if it is not */
void *
meta_find(name)
	char *name;
{
	struct metafn *f;

	if (!inited)
		init();
	for (f = fns; f->name; f++)
		if (strcmp(f->name, name) == 0)
			return (void *)f;
	return 0;
}

static struct metafn *
bycode(code)
	int code;
{
	struct metafn *f;

	if (!inited)
		init();
	for (f = fns; f->name; f++)
		if (f->code == code)
			return f;
	return 0;
}

u32 meta_record();

/* a GDI function of ours, to play a record with: recorded if the DC is a metafile's (one played into another) */
static u32
call(name, a)
	char *name;
	u32 *a;
{
	apifn fn;
	void *f;

	if (meta_dc(a[0]) && (f = meta_find(name)) != 0)
		return meta_record(f, a);
	fn = api_fn("GDI", name);
	if (w16_debug > 1)
		w16_log("startwin: metafile plays %s(%lx %lx %lx %lx %lx %lx)\n", name, (long)a[0], (long)a[1], (long)a[2],
		    (long)a[3], (long)a[4], (long)a[5]);
	return fn ? (*fn)(a) : 0;
}

/* ---- metafile DCs ---- */

static struct mf *
mfof(h)
	u32 h;
{
	struct gobj *o = gobj(h, 0);

	return o && o->type == OBJ_METADC ? (struct mf *)o->u.dc->meta : 0;
}

int
meta_dc(h)
	u32 h;
{
	return mfof(h) != 0;
}

static void
grow(m, n)
	struct mf *m;
	long n;
{
	while (m->n + n > m->cap) {
		m->cap = m->cap ? m->cap * 2 : 4096;
		m->b = (u8 *)realloc(m->b, m->cap);
	}
}

static void
word(m, w)
	struct mf *m;
	u32 w;
{
	grow(m, 2L);
	m->b[m->n] = w;
	m->b[m->n + 1] = w >> 8;
	m->n += 2;
}

static void
bytes(m, p, n)
	struct mf *m;
	u8 *p;
	long n;
{
	grow(m, n + 1);
	memcpy(m->b + m->n, p, n);
	m->n += n;
	if (n & 1)
		m->b[m->n++] = 0;
}

static void
begin(m, code)
	struct mf *m;
	int code;
{
	m->rec = m->n;
	word(m, 0);
	word(m, 0);
	word(m, code);
}

static void
end(m)
	struct mf *m;
{
	long w = (m->n - m->rec) / 2;

	m->b[m->rec] = w;
	m->b[m->rec + 1] = w >> 8;
	m->b[m->rec + 2] = w >> 16;
	m->b[m->rec + 3] = w >> 24;
	if (w > m->maxrec)
		m->maxrec = w;
}

/* the arguments after the DC, as they are on the stack: the last first, a long's low word first */
static void
stackwords(m, args, a)
	struct mf *m;
	char *args;
	u32 *a;
{
	int k;

	for (k = strlen(args) - 1; k >= 1; k--)
		if (args[k] == 'l' || args[k] == 'p') {
			word(m, a[k] & 0xffff);
			word(m, a[k] >> 16);
		} else
			word(m, a[k] & 0xffff);
}

/* a free slot of the metafile's table for an object */
static int
newslot(m, h)
	struct mf *m;
	u32 h;
{
	int i;

	for (i = 0; i < NSLOT && m->slot[i]; i++)
		;
	if (i == NSLOT)
		return -1;
	m->slot[i] = h;
	if (i + 1 > m->nslot)
		m->nslot = i + 1;
	return i;
}

static void
dropslot(m, i)
	struct mf *m;
	int i;
{
	begin(m, META_DELETEOBJECT);
	word(m, i);
	end(m);
	m->slot[i] = 0;
}

/* a surface rectangle as a DIB of 8 bits with the system palette, bottom row first */
static void
dib8(m, s, x, y, w, h)
	struct mf *m;
	struct surf *s;
	int x, y, w, h;
{
	int i, j, rowb = (w + 3) & ~3, px, py;
	u8 *row;

	word(m, 40); word(m, 0);
	word(m, w); word(m, 0);
	word(m, h); word(m, 0);
	word(m, 1);
	word(m, 8);
	for (i = 0; i < 6; i++) {
		word(m, i == 2 ? rowb * h : 0);
		word(m, (i == 2 ? (long)rowb * h : 0) >> 16);
	}
	for (i = 0; i < 256; i++) {
		u8 q[4];

		if (s->mono)
			q[0] = q[1] = q[2] = i ? 255 : 0;
		else {
			q[0] = syspal[i][2];
			q[1] = syspal[i][1];
			q[2] = syspal[i][0];
		}
		q[3] = 0;
		bytes(m, q, 4L);
	}
	row = (u8 *)calloc(1, rowb);
	for (j = h - 1; j >= 0; j--) {
		py = y + j;
		for (i = 0; i < w; i++) {
			px = x + i;
			row[i] = px >= 0 && py >= 0 && px < s->w && py < s->h ? s->pix[py * s->rowb + px] : 0;
		}
		bytes(m, row, (long)rowb);
	}
	free(row);
}

/* the size of a packed DIB (BITMAPINFO and bits) at linear address p, of lines scan lines (-1 all) */
static long
dibsize(p, usage, lines)
	u32 p;
	int usage;
	long lines;
{
	long hs = GL(p), w, h, bpp, nc, rowb;

	if (hs == 12) {
		w = GW(p + 4);
		h = (short)GW(p + 6);
		bpp = GW(p + 10);
		nc = bpp <= 8 ? 1L << bpp : 0;
		hs += nc * (usage ? 2 : 3);
	} else {
		w = GL(p + 4);
		h = (long)GL(p + 8);
		bpp = GW(p + 14);
		nc = GL(p + 32) ? GL(p + 32) : bpp <= 8 ? 1L << bpp : 0;
		hs += nc * (usage ? 2 : 4);
	}
	if (h < 0)
		h = -h;
	if (lines >= 0 && lines < h)
		h = lines;
	rowb = ((w * bpp + 31) / 32) * 4;
	return hs + rowb * h;
}

/* an object's create record, from what it is */
static void
create(m, h)
	struct mf *m;
	u32 h;
{
	struct gobj *o = gobj(h, 0);
	struct logfont *lf;
	int i;

	if (!o)
		return;
	switch (o->type) {
	case OBJ_PEN:
		begin(m, META_CREATEPENINDIRECT);
		word(m, o->u.pen.style);
		word(m, o->u.pen.width);
		word(m, 0);
		word(m, o->u.pen.color & 0xffff);
		word(m, o->u.pen.color >> 16);
		end(m);
		break;
	case OBJ_BRUSH:
		if (o->u.brush.style == 3 || o->u.brush.style == 5) {
			/* a pattern: as a DIB, its 8x8 pixels */
			begin(m, META_DIBCREATEPATTERNBRUSH);
			word(m, 5);
			word(m, 0);
			{
				struct surf s;

				s.w = s.h = s.rowb = 8;
				s.pix = o->u.brush.pat;
				s.mono = o->u.brush.monopat;
				dib8(m, &s, 0, 0, 8, 8);
			}
			end(m);
			break;
		}
		begin(m, META_CREATEBRUSHINDIRECT);
		word(m, o->u.brush.style);
		word(m, o->u.brush.color & 0xffff);
		word(m, o->u.brush.color >> 16);
		word(m, o->u.brush.hatch);
		end(m);
		break;
	case OBJ_FONT:
		lf = &o->u.font.lf;
		begin(m, META_CREATEFONTINDIRECT);
		word(m, lf->height);
		word(m, lf->width);
		word(m, lf->escapement);
		word(m, lf->orientation);
		word(m, lf->weight);
		word(m, lf->italic | lf->underline << 8);
		word(m, lf->strikeout | lf->charset << 8);
		word(m, lf->outprec | lf->clipprec << 8);
		word(m, lf->quality | lf->pitchfam << 8);
		{
			u8 face[32];

			memset(face, 0, sizeof face);
			strncpy((char *)face, lf->face, 31);
			bytes(m, face, 32L);
		}
		end(m);
		break;
	case OBJ_RGN:
		{
			struct rgn *g = &o->u.rgn;
			long nscan = 0, maxscan = 0, start;
			int j, k;

			begin(m, META_CREATEREGION);
			start = m->n;
			for (i = 0; i < 11; i++)
				word(m, 0);
			for (i = 0; i < g->n; i = j) {
				for (j = i; j < g->n && g->r[j].t == g->r[i].t && g->r[j].b == g->r[i].b; j++)
					;
				word(m, 2 * (j - i));
				word(m, g->r[i].t);
				word(m, g->r[i].b);
				for (k = i; k < j; k++) {
					word(m, g->r[k].l);
					word(m, g->r[k].r);
				}
				word(m, 2 * (j - i));
				nscan++;
				if (2 * (j - i) > maxscan)
					maxscan = 2 * (j - i);
			}
			/* the header: next, type 6, the object's size, scans, the widest, the bounds */
			m->b[start + 2] = 6;
			m->b[start + 8] = (m->n - start) & 0xff;
			m->b[start + 9] = (m->n - start) >> 8;
			m->b[start + 10] = nscan;
			m->b[start + 11] = nscan >> 8;
			m->b[start + 12] = maxscan;
			m->b[start + 13] = maxscan >> 8;
			m->b[start + 14] = g->box.l; m->b[start + 15] = g->box.l >> 8;
			m->b[start + 16] = g->box.t; m->b[start + 17] = g->box.t >> 8;
			m->b[start + 18] = g->box.r; m->b[start + 19] = g->box.r >> 8;
			m->b[start + 20] = g->box.b; m->b[start + 21] = g->box.b >> 8;
			end(m);
		}
		break;
	case OBJ_PAL:
		{
			u32 a[4], p = ualloc(4 + 4 * 256), l = ulin(p);
			int n;

			a[0] = h;
			a[1] = 0;
			a[2] = 256;
			a[3] = FP(FPSEL(p), FPOFF(p) + 4);
			n = (int)call("GetPaletteEntries", a);
			begin(m, META_CREATEPALETTE);
			word(m, 0x300);
			word(m, n);
			bytes(m, M + l + 4, 4L * n);
			end(m);
			ufree(p);
		}
		break;
	}
}

/* an object in the table, created there if it is not yet: its slot */
static int
slotof(m, h)
	struct mf *m;
	u32 h;
{
	int i;

	h &= 0xffff;
	for (i = 0; i < m->nslot; i++)
		if (m->slot[i] == h)
			return i;
	if ((i = newslot(m, h)) < 0)
		return -1;
	create(m, h);
	return i;
}

/* an object deleted: gone from the tables of the metafiles being recorded too */
void
meta_objgone(h)
	u32 h;
{
	extern struct gobj *gobj_at();
	extern int gobj_max();
	struct gobj *o;
	struct mf *m;
	int i, k;

	h &= 0xffff;
	for (k = 0; k < gobj_max(); k++) {
		if ((o = gobj_at(k)) == 0 || o->type != OBJ_METADC || !(m = (struct mf *)o->u.dc->meta))
			continue;
		for (i = 0; i < m->nslot; i++)
			if (m->slot[i] == h)
				dropslot(m, i);
	}
}

/* a string or other guest bytes at a far pointer, n of them */
static u8 *
gbytes(fp, n)
	u32 fp;
	long n;
{
	u32 l = lin(FPSEL(fp), FPOFF(fp));

	return l && n > 0 ? M + l : 0;
}

/* a GDI call on a metafile DC: its record; what the call gives back */
u32
meta_record(fp, a)
	void *fp;
	u32 *a;
{
	struct metafn *f = (struct metafn *)fp;
	struct mf *m = mfof(a[0]);
	struct dc *src;
	u8 *p;
	int n, i, s;

	if (!m || !f->args)
		return 0;
	switch (f->kind) {
	case K_PLAIN:
		begin(m, f->code);
		stackwords(m, f->args, a);
		end(m);
		break;
	case K_TEXTOUT:
		/* TextOut(hdc, x, y, str, n): count, the text, y, x */
		n = (short)a[4];
		if (n <= 0 || (p = gbytes(a[3], (long)n)) == 0)
			return 1;
		begin(m, f->code);
		word(m, n);
		bytes(m, p, (long)n);
		word(m, a[2]);
		word(m, a[1]);
		end(m);
		break;
	case K_EXTTEXTOUT:
		/* ExtTextOut(hdc, x, y, opts, rect, str, n, dx): y, x, count, options, rect if one, text, dx if any */
		n = (short)a[6];
		if (n < 0)
			n = 0;
		begin(m, f->code);
		word(m, a[2]);
		word(m, a[1]);
		word(m, n);
		word(m, a[3]);
		if ((a[3] & 6) && (p = gbytes(a[4], 8L)) != 0)
			bytes(m, p, 8L);
		if (n && (p = gbytes(a[5], (long)n)) != 0)
			bytes(m, p, (long)n);
		if (n && (p = gbytes(a[7], 2L * n)) != 0)
			bytes(m, p, 2L * n);
		end(m);
		break;
	case K_POLY:
		/* Polygon(hdc, points, n): count, points */
		n = (short)a[2];
		if (n <= 0 || (p = gbytes(a[1], 4L * n)) == 0)
			return 1;
		begin(m, f->code);
		word(m, n);
		bytes(m, p, 4L * n);
		end(m);
		break;
	case K_POLYPOLY:
		/* PolyPolygon(hdc, points, counts, n): n, the counts, points */
		{
			long total = 0;
			u8 *c;

			n = (short)a[3];
			if (n <= 0 || (c = gbytes(a[2], 2L * n)) == 0)
				return 1;
			for (i = 0; i < n; i++)
				total += (short)(c[2 * i] | c[2 * i + 1] << 8);
			if ((p = gbytes(a[1], 4 * total)) == 0)
				return 1;
			begin(m, f->code);
			word(m, n);
			bytes(m, c, 2L * n);
			bytes(m, p, 4 * total);
			end(m);
		}
		break;
	case K_SELECT:
		/* SelectObject(hdc, h): the object made in the table, then chosen; a region is the clip region */
		{
			struct gobj *o = gobj(a[1], 0);

			if (!o)
				return 0;
			if (o->type == OBJ_RGN) {
				u32 b[2];

				b[0] = a[0];
				b[1] = a[1];
				meta_record(meta_find("SelectClipRgn"), b);
				return 2;
			}
			if ((s = slotof(m, a[1])) < 0)
				return 0;
			begin(m, META_SELECTOBJECT);
			word(m, s);
			end(m);
			/* the "old" object: the DC's starting one of the kind */
			return o->type == OBJ_PEN ? stockobj[BLACK_PEN] : o->type == OBJ_BRUSH ? stockobj[WHITE_BRUSH] :
			    o->type == OBJ_FONT ? stockobj[SYSTEM_FONT] : a[1];
		}
	case K_SELPAL:
		if ((s = slotof(m, a[1])) < 0)
			return 0;
		begin(m, META_SELECTPALETTE);
		word(m, s);
		end(m);
		return stockobj[DEFAULT_PALETTE];
	case K_FILLRGN:
	case K_FRAMERGN:
	case K_RGN1:
	case K_SELCLIP:
		/* the region made in the table for this, and deleted after */
		{
			int rs, bs = -1;

			if (f->kind == K_SELCLIP && !(a[1] & 0xffff)) {
				begin(m, f->code);
				word(m, 0xffff);
				end(m);
				return 1;
			}
			if ((rs = newslot(m, a[1] & 0xffff)) < 0)
				return 0;
			create(m, a[1]);
			if (f->kind == K_FILLRGN || f->kind == K_FRAMERGN)
				bs = slotof(m, a[2]);
			begin(m, f->code);
			if (f->kind == K_FRAMERGN) {
				word(m, a[4]);
				word(m, a[3]);
			}
			if (bs >= 0)
				word(m, bs);
			word(m, rs);
			end(m);
			dropslot(m, rs);
		}
		break;
	case K_BITBLT:
	case K_STRETCHBLT:
		/*
		 * BitBlt(hdc, x, y, w, h, src, sx, sy, rop): rop, sy, sx, h, w, y, x and the
		 * source's DIB; StretchBlt's has the source's size too; without a source the
		 * DC's place is 0.
		 */
		{
			int st = f->kind == K_STRETCHBLT, ai = st ? 10 : 8;
			u32 rop = a[ai];
			int needsrc = ((rop >> 2) ^ rop) & 0x330000L;
			int sw = st ? (short)a[8] : (short)a[3], sh = st ? (short)a[9] : (short)a[4];

			src = needsrc ? dc_get(a[5]) : 0;
			begin(m, f->code);
			word(m, rop & 0xffff);
			word(m, rop >> 16);
			if (st) {
				word(m, sh);
				word(m, sw);
			}
			word(m, a[7]);
			word(m, a[6]);
			if (!src)
				word(m, 0);
			word(m, a[4]);
			word(m, a[3]);
			word(m, a[2]);
			word(m, a[1]);
			if (src) {
				int sx = (short)a[6], sy = (short)a[7];

				lp2dp(src, &sx, &sy);
				dib8(m, src->s, sx + src->ox, sy + src->oy, sw < 0 ? -sw : sw, sh < 0 ? -sh : sh);
			}
			end(m);
		}
		break;
	case K_STRETCHDIB:
		/* StretchDIBits(hdc, x, y, w, h, sx, sy, sw, sh, bits, bmi, usage, rop) */
		{
			u32 bi = lin(FPSEL(a[10]), FPOFF(a[10])), bl = lin(FPSEL(a[9]), FPOFF(a[9]));
			long hs, size;

			if (!bi)
				return 0;
			hs = dibsize(bi, (int)(a[11] & 0xffff), 0L);
			size = dibsize(bi, (int)(a[11] & 0xffff), -1L);
			begin(m, f->code);
			word(m, a[12] & 0xffff);
			word(m, a[12] >> 16);
			for (i = 11; i >= 1; i--)
				if (i != 9 && i != 10)
					word(m, a[i]);
			bytes(m, M + bi, hs);
			if (bl)
				bytes(m, M + bl, size - hs);
			end(m);
		}
		break;
	case K_SETDIB:
		/* SetDIBitsToDevice(hdc, x, y, w, h, sx, sy, start, lines, bits, bmi, usage) */
		{
			u32 bi = lin(FPSEL(a[10]), FPOFF(a[10])), bl = lin(FPSEL(a[9]), FPOFF(a[9]));
			long all, hs, lines = a[8] & 0xffff;

			if (!bi)
				return 0;
			all = dibsize(bi, (int)(a[11] & 0xffff), 0L);
			hs = all;
			all = dibsize(bi, (int)(a[11] & 0xffff), lines);
			begin(m, f->code);
			for (i = 11; i >= 1; i--)
				if (i != 9 && i != 10)
					word(m, a[i]);
			bytes(m, M + bi, hs);
			if (bl)
				bytes(m, M + bl, all - hs);
			end(m);
			return lines;
		}
	case K_ESCAPE:
		/* Escape(hdc, n, size, in, out): n, size, what came in; asking what is done is not recorded */
		n = (short)a[2];
		if ((a[1] & 0xffff) == 8)
			return 0;
		begin(m, f->code);
		word(m, a[1]);
		word(m, n);
		if (n > 0 && (p = gbytes(a[3], (long)n)) != 0)
			bytes(m, p, (long)n);
		end(m);
		break;
	}
	return f->ret;
}

/* ---- making and closing them ---- */

static u32
g_CreateMetaFile(a)
	u32 *a;
{
	u16 h = dc_new(DCK_INFO);
	struct dc *dc = dc_get(h);
	struct mf *m = (struct mf *)calloc(1, sizeof *m);
	char *name = a[0] ? gptr(a[0]) : 0;
	int i;

	gobj(h, 0)->type = OBJ_METADC;
	dc->s = &screen;
	dc->meta = (void *)m;
	if (name)
		strncpy(m->path, name, sizeof m->path - 1);
	/* the header, filled in at the end */
	for (i = 0; i < 9; i++)
		word(m, 0);
	return h;
}

/* a metafile's bytes into a new global block: its handle */
static u16
toblock(b, n)
	u8 *b;
	long n;
{
	u16 h = g_alloc(GMEM_MOVEABLE | GMEM_DDESHARE, (u32)n, 0);

	if (h)
		memcpy(M + sel_base(h), b, n);
	return h;
}

static u32
g_CloseMetaFile(a)
	u32 *a;
{
	struct mf *m = mfof(a[0]);
	struct gobj *o = gobj(a[0], 0);
	u16 h;
	int i;

	if (!m)
		return 0;
	begin(m, META_EOF);
	end(m);
	m->b[0] = m->path[0] ? 2 : 1;
	m->b[2] = 9;
	m->b[4] = 0; m->b[5] = 3;
	for (i = 0; i < 4; i++)
		m->b[6 + i] = (m->n / 2) >> (8 * i);
	m->b[10] = m->nslot;
	m->b[11] = m->nslot >> 8;
	for (i = 0; i < 4; i++)
		m->b[12 + i] = m->maxrec >> (8 * i);
	if (m->path[0]) {
		char host[1024];
		FILE *fp;

		if (dos_hostpath(m->path, host, sizeof host, 1) == 0 && (fp = fopen(host, "wb")) != 0) {
			fwrite(m->b, 1, m->n, fp);
			fclose(fp);
		}
	}
	h = toblock(m->b, m->n);
	free(m->b);
	free(m);
	o->u.dc->meta = 0;
	o->type = OBJ_DC;
	dc_free(a[0]);
	return h;
}

/* GetMetaFile(file): read in */
static u32
g_GetMetaFile(a)
	u32 *a;
{
	char *name = gptr(a[0]), host[1024];
	FILE *fp;
	long n;
	u8 *b;
	u16 h;

	if (!name || dos_hostpath(name, host, sizeof host, 0) != 0 || (fp = fopen(host, "rb")) == 0)
		return 0;
	fseek(fp, 0L, 2);
	n = ftell(fp);
	fseek(fp, 0L, 0);
	if (n < 18 || (b = (u8 *)malloc(n)) == 0) {
		fclose(fp);
		return 0;
	}
	n = fread(b, 1, n, fp);
	fclose(fp);
	b[0] = 1;	/* in memory now */
	h = toblock(b, n);
	free(b);
	return h;
}

static u32
g_DeleteMetaFile(a)
	u32 *a;
{
	return g_free(a[0]) == 0;
}

/* GetMetaFileBits(hmf): the same block, the program's now */
static u32
g_GetMetaFileBits(a)
	u32 *a;
{
	return a[0] & 0xffff;
}

/* SetMetaFileBits(hmem): a metafile of it */
static u32
g_SetMetaFileBits(a)
	u32 *a;
{
	u32 b = g_block(a[0]) ? sel_base(a[0]) : 0;

	if (!b || (GW(b) != 1 && GW(b) != 2) || GW(b + 2) != 9)
		return 0;
	return a[0] & 0xffff;
}

/* CopyMetaFile(hmf, file): a copy, written there too if named */
static u32
g_CopyMetaFile(a)
	u32 *a;
{
	u32 b = g_block(a[0]) ? sel_base(a[0]) : 0;
	long n;
	char *name = a[1] ? gptr(a[1]) : 0, host[1024];
	FILE *fp;

	if (!b)
		return 0;
	n = GL(b + 6) * 2;
	if (n < 18 || n > (long)g_size(a[0]))
		n = g_size(a[0]);
	if (name && dos_hostpath(name, host, sizeof host, 1) == 0 && (fp = fopen(host, "wb")) != 0) {
		fwrite(M + b, 1, n, fp);
		fclose(fp);
	}
	return toblock(M + b, n);
}

static u32
g_IsValidMetaFile(a)
	u32 *a;
{
	u32 b = g_block(a[0]) ? sel_base(a[0]) : 0;

	return b && (GW(b) == 1 || GW(b) == 2) && GW(b + 2) == 9 && GW(b + 4) >= 0x100;
}

/* ---- playing ---- */

/* a far pointer to byte off of a block (a huge one's selectors are 8 apart) */
static u32
fpat(h, off)
	u32 h, off;
{
	return FP((h & 0xffff) + (off >> 16) * 8, off & 0xffff);
}

static int
tableput(ht, n, h)
	u32 ht;
	int n;
	u32 h;
{
	int i;

	for (i = 0; i < n; i++)
		if (!GW(ht + 2 * i)) {
			PW(ht + 2 * i, h);
			return i;
		}
	return -1;
}

static u32
tableget(ht, n, i)
	u32 ht;
	int n, i;
{
	return i >= 0 && i < n ? GW(ht + 2 * i) : 0;
}

/* one record (far pointer mr), with the handle table (linear ht, n slots) */
static void
play(hdc, ht, n, mr)
	u32 hdc, ht, mr;
	int n;
{
	u32 l = lin(FPSEL(mr), FPOFF(mr)), a[16], size;
	struct metafn *f;
	int code, i, k, w;

#define	P(i)	((u32)GW(l + 6 + 2 * (i)))
#define	SP(i)	((u32)(s32)(short)GW(l + 6 + 2 * (i)))
#define	PFP(i)	FP(FPSEL(mr), FPOFF(mr) + 6 + 2 * (i))

	if (!l)
		return;
	size = GL(l);
	code = GW(l + 4);
	memset((char *)a, 0, sizeof a);
	a[0] = hdc;
	switch (code) {
	case META_EOF:
		return;
	case META_TEXTOUT:
		k = P(0);
		w = (k + 1) / 2;
		a[1] = SP(1 + w + 1);
		a[2] = SP(1 + w);
		a[3] = PFP(1);
		a[4] = k;
		call("TextOut", a);
		return;
	case META_EXTTEXTOUT:
		k = P(2);
		i = 4;
		a[1] = SP(1);
		a[2] = SP(0);
		a[3] = P(3);
		if (P(3) & 6) {
			a[4] = PFP(4);
			i = 8;
		}
		a[5] = PFP(i);
		a[6] = k;
		/* the widths if the record has room for them */
		if ((long)size >= 3 + i + (k + 1) / 2 + k)
			a[7] = PFP(i + (k + 1) / 2);
		call("ExtTextOut", a);
		return;
	case META_POLYGON:
	case META_POLYLINE:
		a[1] = PFP(1);
		a[2] = P(0);
		call(code == META_POLYGON ? "Polygon" : "Polyline", a);
		return;
	case META_POLYPOLYGON:
		a[3] = P(0);
		a[2] = PFP(1);
		a[1] = PFP(1 + P(0));
		call("PolyPolygon", a);
		return;
	case META_SELECTOBJECT:
		a[1] = tableget(ht, n, (int)P(0));
		if (a[1])
			call("SelectObject", a);
		return;
	case META_DELETEOBJECT:
		a[0] = tableget(ht, n, (int)P(0));
		if (a[0])
			call("DeleteObject", a);
		if ((int)P(0) < n)
			PW(ht + 2 * P(0), 0);
		return;
	case META_CREATEPENINDIRECT:
		a[0] = PFP(0);
		tableput(ht, n, call("CreatePenIndirect", a));
		return;
	case META_CREATEBRUSHINDIRECT:
		a[0] = PFP(0);
		tableput(ht, n, call("CreateBrushIndirect", a));
		return;
	case META_CREATEFONTINDIRECT:
		a[0] = PFP(0);
		tableput(ht, n, call("CreateFontIndirect", a));
		return;
	case META_CREATEPALETTE:
		a[0] = PFP(0);
		tableput(ht, n, call("CreatePalette", a));
		return;
	case META_DIBCREATEPATTERNBRUSH:
		/* from a copy of the DIB in a block of its own, as CreateDIBPatternBrush takes it */
		{
			long ds = (size - 5) * 2;
			u16 hm = g_alloc(GMEM_MOVEABLE, (u32)ds, 0);

			if (!hm)
				return;
			memcpy(M + sel_base(hm), M + l + 10, ds);
			a[0] = hm;
			a[1] = P(1);
			tableput(ht, n, call("CreateDIBPatternBrush", a));
			g_free(hm);
		}
		return;
	case META_CREATEPATTERNBRUSH:
		/* the old kind: a BITMAP and its bits */
		{
			u32 b[5], hb;

			b[0] = P(0);
			b[1] = P(1);
			b[2] = P(3) & 0xff;
			b[3] = P(3) >> 8;
			b[4] = PFP(7);
			if ((hb = call("CreateBitmap", b)) != 0) {
				b[0] = hb;
				tableput(ht, n, call("CreatePatternBrush", b));
				b[0] = hb;
				call("DeleteObject", b);
			}
		}
		return;
	case META_CREATEREGION:
		{
			u32 r, t[5];
			int band, pairs, pos = 11, j;

			t[0] = t[1] = t[2] = t[3] = 0;
			r = call("CreateRectRgn", t);
			for (band = 0; band < (int)P(5) && 3 + pos < (int)size; band++) {
				pairs = P(pos) / 2;
				for (j = 0; j < pairs; j++) {
					u32 c[4], q;

					c[0] = SP(pos + 3 + 2 * j);
					c[1] = SP(pos + 1);
					c[2] = SP(pos + 4 + 2 * j);
					c[3] = SP(pos + 2);
					q = call("CreateRectRgn", c);
					t[0] = r;
					t[1] = r;
					t[2] = q;
					t[3] = 2;	/* RGN_OR */
					call("CombineRgn", t);
					t[0] = q;
					call("DeleteObject", t);
				}
				pos += 2 * pairs + 4;
			}
			tableput(ht, n, r);
		}
		return;
	case META_SELECTCLIPREGION:
		a[1] = P(0) == 0xffff ? 0 : tableget(ht, n, (int)P(0));
		call("SelectClipRgn", a);
		return;
	case META_FILLREGION:
		a[1] = tableget(ht, n, (int)P(1));
		a[2] = tableget(ht, n, (int)P(0));
		call("FillRgn", a);
		return;
	case META_FRAMEREGION:
		a[1] = tableget(ht, n, (int)P(3));
		a[2] = tableget(ht, n, (int)P(2));
		a[3] = SP(1);
		a[4] = SP(0);
		call("FrameRgn", a);
		return;
	case META_INVERTREGION:
	case META_PAINTREGION:
		a[1] = tableget(ht, n, (int)P(0));
		call(code == META_INVERTREGION ? "InvertRgn" : "PaintRgn", a);
		return;
	case META_SELECTPALETTE:
		a[1] = tableget(ht, n, (int)P(0));
		a[2] = 0;
		call("SelectPalette", a);
		return;
	case META_SETPALENTRIES:
	case META_ANIMATEPALETTE:
	case META_RESIZEPALETTE:
		{
			struct dc *dc = dc_get(hdc);

			if (!dc)
				return;
			a[0] = dc->st.pal;
			if (code == META_RESIZEPALETTE) {
				a[1] = P(0);
				call("ResizePalette", a);
				return;
			}
			a[1] = P(0);
			a[2] = P(1);
			a[3] = PFP(2);
			call(code == META_SETPALENTRIES ? "SetPaletteEntries" : "AnimatePalette", a);
		}
		return;
	case META_ESCAPE:
		a[1] = P(0);
		a[2] = P(1);
		a[3] = PFP(2);
		a[4] = 0;
		call("Escape", a);
		return;
	case META_DIBBITBLT:
		if (size > 12) {
			/* StretchDIBits(hdc, x, y, w, h, sx, sy, sw, sh, bits, bmi, usage, rop) */
			u32 bi = l + 6 + 16;

			a[1] = SP(7); a[2] = SP(6); a[3] = SP(5); a[4] = SP(4);
			a[5] = SP(3); a[6] = SP(2); a[7] = SP(5); a[8] = SP(4);
			a[10] = PFP(8);
			a[9] = FP(FPSEL(mr), FPOFF(mr) + 6 + 16 + (dibsize(bi, 0, 0L)));
			a[11] = 0;
			a[12] = P(0) | P(1) << 16;
			call("StretchDIBits", a);
		} else {
			a[1] = SP(8); a[2] = SP(7); a[3] = SP(6); a[4] = SP(5);
			a[5] = P(0) | P(1) << 16;
			call("PatBlt", a);
		}
		return;
	case META_DIBSTRETCHBLT:
		if (size > 14) {
			u32 bi = l + 6 + 20;

			a[1] = SP(9); a[2] = SP(8); a[3] = SP(7); a[4] = SP(6);
			a[5] = SP(5); a[6] = SP(4); a[7] = SP(3); a[8] = SP(2);
			a[10] = PFP(10);
			a[9] = FP(FPSEL(mr), FPOFF(mr) + 6 + 20 + (dibsize(bi, 0, 0L)));
			a[11] = 0;
			a[12] = P(0) | P(1) << 16;
			call("StretchDIBits", a);
		} else {
			a[1] = SP(10); a[2] = SP(9); a[3] = SP(8); a[4] = SP(7);
			a[5] = P(0) | P(1) << 16;
			call("PatBlt", a);
		}
		return;
	case META_STRETCHDIB:
		{
			u32 bi = l + 6 + 22;

			a[1] = SP(10); a[2] = SP(9); a[3] = SP(8); a[4] = SP(7);
			a[5] = SP(6); a[6] = SP(5); a[7] = SP(4); a[8] = SP(3);
			a[11] = P(2);
			a[10] = PFP(11);
			a[9] = FP(FPSEL(mr), FPOFF(mr) + 6 + 22 + (dibsize(bi, (int)P(2), 0L)));
			a[12] = P(0) | P(1) << 16;
			call("StretchDIBits", a);
		}
		return;
	case META_SETDIBTODEV:
		{
			u32 bi = l + 6 + 18;

			a[1] = SP(8); a[2] = SP(7); a[3] = SP(6); a[4] = SP(5);
			a[5] = SP(4); a[6] = SP(3); a[7] = P(2); a[8] = P(1);
			a[11] = P(0);
			a[10] = PFP(9);
			a[9] = FP(FPSEL(mr), FPOFF(mr) + 6 + 18 + (dibsize(bi, (int)P(0), 0L)));
			call("SetDIBitsToDevice", a);
		}
		return;
	case META_BITBLT:
	case META_STRETCHBLT:
		/* the old kind: a device-dependent BITMAP in the record, through a memory DC */
		{
			int st = code == META_STRETCHBLT, o = st ? 10 : 8;
			u32 b[13], hb, hm, old;

			b[0] = P(o);
			b[1] = P(o + 1);
			b[2] = P(o + 3) & 0xff;
			b[3] = P(o + 3) >> 8;
			b[4] = PFP(o + 7);
			if ((hb = call("CreateBitmap", b)) == 0)
				return;
			b[0] = hdc;
			hm = call("CreateCompatibleDC", b);
			b[0] = hm;
			b[1] = hb;
			old = call("SelectObject", b);
			a[0] = hdc;
			if (st) {
				a[1] = SP(9); a[2] = SP(8); a[3] = SP(7); a[4] = SP(6);
				a[5] = hm;
				a[6] = SP(5); a[7] = SP(4); a[8] = SP(3); a[9] = SP(2);
				a[10] = P(0) | P(1) << 16;
				call("StretchBlt", a);
			} else {
				a[1] = SP(7); a[2] = SP(6); a[3] = SP(5); a[4] = SP(4);
				a[5] = hm;
				a[6] = SP(3); a[7] = SP(2);
				a[8] = P(0) | P(1) << 16;
				call("BitBlt", a);
			}
			b[0] = hm;
			b[1] = old;
			call("SelectObject", b);
			call("DeleteDC", b);
			b[0] = hb;
			call("DeleteObject", b);
		}
		return;
	}
	/* the plain ones: their arguments as they were on the stack */
	if ((f = bycode(code)) == 0 || f->kind != K_PLAIN || !f->args || !f->fn) {
		if (w16_debug)
			w16_log("startwin: metafile record %04x not played\n", code);
		return;
	}
	for (k = strlen(f->args) - 1, i = 0; k >= 1 && k < 16; k--)
		if (f->args[k] == 'l' || f->args[k] == 'p') {
			a[k] = P(i) | P(i + 1) << 16;
			i += 2;
		} else {
			a[k] = f->args[k] == 's' ? SP(i) : P(i);
			i++;
		}
	call(f->name, a);
#undef	P
#undef	SP
#undef	PFP
}

/*
 * The records of a metafile one by one: to cb (EnumMetaFile's procedure:
 * hdc, handle table, record, objects, data; 0 stops) or played.
 */
static u32
walk(hdc, hmf, cb, data)
	u32 hdc, hmf, cb, data;
{
	u32 b = g_block(hmf) ? sel_base(hmf) : 0, ht, htp, off, words, size, r = 1;
	int n;
	u32 a[1];

	if (!b || GW(b + 2) != 9)
		return 0;
	words = GL(b + 6);
	n = GW(b + 10);
	if (n < 1)
		n = 1;
	htp = ualloc((u32)n * 2);
	ht = ulin(htp);
	a[0] = hdc;
	call("SaveDC", a);
	for (off = 18; off + 6 <= words * 2 && off + 6 <= g_size(hmf); off += size * 2) {
		u32 lr = lin(FPSEL(fpat(hmf, off)), FPOFF(fpat(hmf, off)));

		size = GL(lr);
		if (size < 3 || GW(lr + 4) == META_EOF)
			break;
		if (cb) {
			cb_begin();
			cb_push16(hdc);
			cb_push32(htp);
			cb_push32(fpat(hmf, off));
			cb_push16(n);
			cb_push32(data);
			if (!(cb_call(cb, 0) & 0xffff)) {
				r = 0;
				break;
			}
		} else
			play(hdc, ht, n, fpat(hmf, off));
		/* the block may have moved: it is the program's */
		b = g_block(hmf) ? sel_base(hmf) : 0;
		if (!b)
			break;
	}
	a[0] = hdc;
	{
		u32 c[2];

		c[0] = hdc;
		c[1] = (u32)-1;
		call("RestoreDC", c);
	}
	/* the objects it made go with it */
	{
		int i;

		for (i = 0; i < n; i++)
			if (GW(ht + 2 * i)) {
				a[0] = GW(ht + 2 * i);
				call("DeleteObject", a);
			}
	}
	ufree(htp);
	return r;
}

static u32
g_PlayMetaFile(a)
	u32 *a;
{
	return walk(a[0], a[1], (u32)0, (u32)0);
}

/* EnumMetaFile(hdc, hmf, proc, data) */
static u32
g_EnumMetaFile(a)
	u32 *a;
{
	return walk(a[0], a[1], a[2], a[3]);
}

/* PlayMetaFileRecord(hdc, handle table, record, objects) */
static u32
g_PlayMetaFileRecord(a)
	u32 *a;
{
	u32 ht = lin(FPSEL(a[1]), FPOFF(a[1]));

	if (!ht)
		return 0;
	play(a[0], ht, (int)(a[3] & 0xffff), a[2]);
	return 1;
}

/* ---- USER's drawing on a metafile DC, as the GDI calls Windows' USER makes ---- */

/* FillRect(hdc, rect, brush): the brush chosen, PatBlt with it (PATCOPY), the first chosen again */
void
meta_fillrect(hdc, l, t, r, b, br)
	u32 hdc, br;
	int l, t, r, b;
{
	u32 a[6], old;

	a[0] = hdc;
	a[1] = br;
	old = call("SelectObject", a);
	a[1] = l; a[2] = t; a[3] = r - l; a[4] = b - t;
	a[5] = 0xf00021L;
	call("PatBlt", a);
	a[1] = old;
	call("SelectObject", a);
}

/* FrameRect: four bars of the brush */
void
meta_framerect(hdc, l, t, r, b, br)
	u32 hdc, br;
	int l, t, r, b;
{
	meta_fillrect(hdc, l, t, r, t + 1, br);
	meta_fillrect(hdc, l, b - 1, r, b, br);
	meta_fillrect(hdc, l, t, l + 1, b, br);
	meta_fillrect(hdc, r - 1, t, r, b, br);
}

/* InvertRect: PatBlt, DSTINVERT */
void
meta_invertrect(hdc, l, t, r, b)
	u32 hdc;
	int l, t, r, b;
{
	u32 a[6];

	a[0] = hdc;
	a[1] = l; a[2] = t; a[3] = r - l; a[4] = b - t;
	a[5] = 0x550009L;
	call("PatBlt", a);
}

/* DrawText: a TextOut a line (a metafile DC knows no font: lines of the system font's height) */
int
meta_drawtext(hdc, s, n, l, t, r, flags)
	u32 hdc;
	char *s;
	int n, l, t, r, flags;
{
	u32 a[5], p;
	int i, start, lh = 16, y = t;

	(void)r;
	(void)flags;
	for (start = 0; start <= n; start = i + 1) {
		for (i = start; i < n && s[i] != '\n' && s[i] != '\r'; i++)
			;
		if (i > start) {
			p = ualloc((u32)(i - start + 1));
			memcpy(M + ulin(p), s + start, i - start);
			a[0] = hdc;
			a[1] = l;
			a[2] = y;
			a[3] = p;
			a[4] = i - start;
			call("TextOut", a);
			ufree(p);
		}
		y += lh;
		if (i < n && s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')
			i++;
	}
	return y - t;
}

struct impl mf_impl[] = {
	{ "GDI", "CreateMetaFile", g_CreateMetaFile },
	{ "GDI", "CloseMetaFile", g_CloseMetaFile },
	{ "GDI", "GetMetaFile", g_GetMetaFile },
	{ "GDI", "DeleteMetaFile", g_DeleteMetaFile },
	{ "GDI", "GetMetaFileBits", g_GetMetaFileBits },
	{ "GDI", "SetMetaFileBits", g_SetMetaFileBits },
	{ "GDI", "SetMetaFileBitsBetter", g_SetMetaFileBits },
	{ "GDI", "CopyMetaFile", g_CopyMetaFile },
	{ "GDI", "IsValidMetaFile", g_IsValidMetaFile },
	{ "GDI", "PlayMetaFile", g_PlayMetaFile },
	{ "GDI", "EnumMetaFile", g_EnumMetaFile },
	{ "GDI", "PlayMetaFileRecord", g_PlayMetaFileRecord },
	{ 0 }
};
