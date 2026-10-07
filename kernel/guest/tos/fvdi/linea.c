/*
 * linea.c -- the Line-A calls on the driver's screen.  The variables
 * describe that screen (8 planes, its pitch, the logical screen at its
 * address), so the screen of a Line-A program is the one GEM draws on.
 * Pixel values are the colour bits; the screen holds one per byte.
 */
#include "fvdi.h"
#include "driver.h"
#include "fbops.h"
#include "osbind.h"

#define	V(o)	(*(short *)(la + (o)))
#define	VL(o)	(*(long *)(la + (o)))
#define	VP(o)	((short *)VL(o))

/* variable offsets from the Line-A base */
#define	PLANES	0
#define	LIN_WR	2
#define	CONTRL	4
#define	INTIN	8
#define	PTSIN	12
#define	COLBIT	24
#define	LSTLIN	32
#define	LN_MASK	34
#define	WMODE	36
#define	X1	38
#define	Y1	40
#define	X2	42
#define	Y2	44
#define	PATPTR	46
#define	PATMSK	50
#define	MFILL	52
#define	CLIP	54
#define	XMINCL	56
#define	YMINCL	58
#define	XMAXCL	60
#define	YMAXCL	62
#define	SOURCEX	72
#define	SOURCEY	74
#define	DESTX	76
#define	DESTY	78
#define	DELX	80
#define	DELY	82
#define	FBASE	84
#define	FWIDTH	88
#define	STYLE	90
#define	LITEMASK 92
#define	SKEWMASK 94
#define	WEIGHT	96
#define	TEXTFG	106
#define	TEXTBG	114
#define	COPYTRAN 116
#define	SEEDABORT 118
#define	BYTES_LIN -2
#define	V_REZ_HZ -12
#define	V_REZ_VT -4
#define	GCURX	-0x25a
#define	GCURY	-0x258
#define	M_HID_CNT -0x256
#define	CUR_FLAG -0x154
#define	V_STAT_0 -6
#define	M_CVIS	4

static Pix **volatile v_bas_ad = (Pix **)0x44e;

static char *la;		/* the Line-A variables */
static long la_fonts;		/* $A000's a1 */
static Pix *scr;
static long wrap, sw, sh;
static unsigned char *seen;	/* seed fill: one bit per screen pixel */
static short st_geom[2], con_geom[2];	/* planes, line width: the console's, the caller's */
static Pix *st_base, *con_base;
static char con_cvis;
static char *alias[2];		/* the program's own screen: physical, logical */

static long (**volatile con_vec)(void) = (long (**)(void))0x57e;	/* Bconout vectors */

void la_trap(void), la_xb(void);
extern long la_old, la_vecs[16], con_old2, con_old5, xb_old;
void la_con2(void), la_con5(void);
long CDECL la_call(long op, long *r);

/* registers as the trap saved them */
enum { D0, D1, D2, A0, A1, A2, A6 };

/*
 * Taken in front of the ROM's handler, which still answers $A000 at
 * start.  Direct calls through the table $A000 returns go to la_call too.
 */
__asm__(
"	.text\n"
"	.long	0x58425241, 0x4173684c\n"	/* XBRA AshL */
"_la_old: .long	0\n"
"	.globl	_la_trap, _la_old, _la_vecs\n"
"_la_trap:\n"
"	movem.l	d0-d2/a0-a2/a6,-(sp)\n"
"	move.l	30(sp),a1\n"
"	addq.l	#2,30(sp)\n"
"	moveq	#15,d0\n"
"	and.w	(a1),d0\n"
"	move.l	sp,-(sp)\n"
"	move.l	d0,-(sp)\n"
"	jsr	_la_call\n"
"	addq.l	#8,sp\n"
"	movem.l	(sp)+,d0-d2/a0-a2/a6\n"
"	rte\n"
"la_direct:\n"
"	movem.l	d0-d2/a0-a2/a6,-(sp)\n"
"	move.l	sp,-(sp)\n"
"	move.l	32(sp),-(sp)\n"
"	jsr	_la_call\n"
"	addq.l	#8,sp\n"
"	movem.l	(sp)+,d0-d2/a0-a2/a6\n"
"	addq.l	#4,sp\n"
"	rts\n"
"	.irp	n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
"la_j\\n:	pea	16+\\n\n"
"	bra.s	la_direct\n"
"	.endr\n"
"	.data\n"
"_la_vecs:\n"
"	.irp	n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
"	.long	la_j\\n\n"
"	.endr\n"
"	.text\n"
/* XBIOS: Setscreen, Physbase and Logbase as la_xbios answers them */
"	.long	0x58425241, 0x4173684c\n"
"_xb_old: .long	0\n"
"	.globl	_la_xb, _xb_old\n"
"_la_xb:\n"
"	move.l	a0,-(sp)\n"
"	lea	10(sp),a0\n"
"	tst.w	0x59e\n"
"	beq.s	1f\n"
"	addq.l	#2,a0\n"
"1:	btst	#5,4(sp)\n"
"	bne.s	2f\n"
"	move.l	usp,a0\n"
"2:	movem.l	d1-d2/a1-a2,-(sp)\n"
"	clr.l	-(sp)\n"
"	pea	(sp)\n"
"	move.l	a0,-(sp)\n"
"	jsr	_la_xbios\n"
"	addq.l	#8,sp\n"
"	move.l	(sp)+,d1\n"
"	tst.l	d0\n"
"	beq.s	3f\n"
"	move.l	d1,d0\n"
"	movem.l	(sp)+,d1-d2/a1-a2\n"
"	move.l	(sp)+,a0\n"
"	rte\n"
"3:	movem.l	(sp)+,d1-d2/a1-a2\n"
"	move.l	(sp)+,a0\n"
"	move.l	_xb_old,-(sp)\n"
"	rts\n"
/*
 * Bconout for the console and raw console: the console draws planes
 * with these variables, so it runs on the screen it had before them.
 */
"	.long	0x58425241, 0x4173684c\n"
"_con_old2: .long	0\n"
"	.globl	_la_con2, _la_con5, _con_old2, _con_old5\n"
"_la_con2:\n"
"	move.l	_con_old2,-(sp)\n"
"	bra.s	la_con\n"
"	.long	0x58425241, 0x4173684c\n"
"_con_old5: .long	0\n"
"_la_con5:\n"
"	move.l	_con_old5,-(sp)\n"
"la_con:\n"
"	jsr	_con_enter\n"
"	move.l	(sp)+,a0\n"
"	move.l	4(sp),-(sp)\n"
"	jsr	(a0)\n"
"	addq.l	#4,sp\n"
"	move.l	d0,-(sp)\n"
"	jsr	_con_leave\n"
"	move.l	(sp)+,d0\n"
"	rts\n");

static const short geo[2] = { PLANES, LIN_WR };

void con_enter(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		con_geom[i] = V(geo[i]);
		V(geo[i]) = st_geom[i];
	}
	con_base = *v_bas_ad;
	*v_bas_ad = st_base;
	la[V_STAT_0] |= con_cvis;
}

/* back on the driver's screen the blinking cursor stays off */
void con_leave(void)
{
	int i, same = con_base == st_base;

	for (i = 0; i < 2; i++) {
		same &= con_geom[i] == st_geom[i];
		V(geo[i]) = con_geom[i];
	}
	*v_bas_ad = con_base;
	con_cvis = same ? 0 : la[V_STAT_0] & M_CVIS;
	la[V_STAT_0] &= ~con_cvis;
}

static long con_hook(void)
{
	int i;

	for (i = 0; i < 2; i++)
		st_geom[i] = V(geo[i]);
	st_base = *v_bas_ad;
	con_old2 = (long)con_vec[2];
	con_old5 = (long)con_vec[5];
	con_vec[2] = (long (*)(void))la_con2;
	con_vec[5] = (long (*)(void))la_con5;
	return 0;
}

void linea_init(Workstation *wk)
{
	register long a0 __asm__("a0");
	register long a1 __asm__("a1");

	__asm__ __volatile__(".word 0xa000" : "=r"(a0), "=r"(a1) : : "d0", "d1", "d2", "a2", "memory");
	la = (char *)a0;
	la_fonts = a1;
	scr = (Pix *)wk->screen.mfdb.address;
	wrap = wk->screen.wrap;
	sw = wk->screen.mfdb.width;
	sh = wk->screen.mfdb.height;
	seen = (unsigned char *)access->funcs.malloc((sw * sh + 7) / 8, 0);
	Supexec((long)con_hook);
	la_old = Setexc(10, (long)la_trap);
	xb_old = Setexc(46, (long)la_xb);
}

/*
 * A program that moves the screen without changing the resolution keeps
 * the frame buffer: its address stands for the frame buffer from then on,
 * so its Line-A and VDI output lands there.  1 if answered here.
 */
long CDECL la_xbios(short *a, long *res)
{
	char *p;
	long k;

	if (a[0] == 2 || a[0] == 3) {
		if (!alias[a[0] - 2])
			return 0;
		*res = (long)alias[a[0] - 2];
		return 1;
	}
	if (a[0] != 5 || a[5] != -1)
		return 0;
	for (k = 0; k < 2; k++) {
		p = *(char **)(a + 3 - 2 * k);
		if (p && p != (char *)-1)
			alias[k] = p == (char *)st_base || p == (char *)scr ? 0 : p;
	}
	return 1;
}

long la_alias(void *a)
{
	return a && (a == alias[0] || a == alias[1]);
}

/* the logical screen and its geometry: the driver's */
static void la_screen(long super)
{
	V(PLANES) = 8;
	V(LIN_WR) = wrap;
	V(BYTES_LIN) = wrap;
	V(V_REZ_HZ) = sw;
	V(V_REZ_VT) = sh;
	if (super)
		*v_bas_ad = scr;
}

static long la_colour(void)
{
	short *c = (short *)(la + COLBIT);

	return (c[0] != 0) | (c[1] != 0) << 1 | (c[2] != 0) << 2 | (c[3] != 0) << 3;
}

/* Line-A writing mode 0-3 as the driver's 1-4 */
static long mode(void)
{
	return (V(WMODE) & 3) + 1;
}

static void solid(unsigned short *pat)
{
	short *p = (short *)VL(PATPTR);
	long i;

	for (i = 0; i < 16; i++)
		pat[i] = p ? p[i & V(PATMSK)] : 0xffff;
}

/* x1..x2, y1..y2 inclusive, clipped by the clip rectangle when asked */
static void rect(long x1, long y1, long x2, long y2, long clip)
{
	unsigned short pat[16];
	short *p;
	long x, y, i, b, w, k;
	Pix *d;

	if (clip) {
		if (x1 < V(XMINCL)) x1 = V(XMINCL);
		if (y1 < V(YMINCL)) y1 = V(YMINCL);
		if (x2 > V(XMAXCL)) x2 = V(XMAXCL);
		if (y2 > V(YMAXCL)) y2 = V(YMAXCL);
	}
	if (x1 < 0) x1 = 0;
	if (y1 < 0) y1 = 0;
	if (x2 >= sw) x2 = sw - 1;
	if (y2 >= sh) y2 = sh - 1;
	if (x1 > x2 || y1 > y2)
		return;
	p = (short *)VL(PATPTR);
	if (!V(MFILL) || !p) {
		solid(pat);
		fb->fill(fb_wk, x1, y1, x2 - x1 + 1, y2 - y1 + 1, pat, la_colour(), 0, mode());
		return;
	}
	/* one pattern per plane, 16 words apart: the pattern is the colour */
	for (y = y1; y <= y2; y++)
		for (x = x1, d = scr + y * wrap + x; x <= x2; x++, d++) {
			k = (y & V(PATMSK));
			for (b = 0, i = 0; i < 8; i++)
				b |= ((unsigned short)p[k + 16 * i] >> (15 - (x & 15)) & 1) << i;
			w = V(WMODE) & 3;
			*d = w == 0 ? b : w == 2 ? *d ^ b : b ? b : *d;
		}
}

static void pixel(long x, long y, long c, long bg, long m, long bit)
{
	Pix *d;

	if (x < 0 || y < 0 || x >= sw || y >= sh)
		return;
	d = scr + y * wrap + x;
	switch (m) {
	case 1: *d = bit ? c : bg; break;
	case 2: if (bit) *d = c; break;
	case 3: if (bit) *d = ~*d; break;
	default: if (!bit) *d = c; break;
	}
}

static void la_line(void)
{
	long x1 = V(X1), y1 = V(Y1), x2 = V(X2), y2 = V(Y2);
	long dx, dy, sx, sy, err, e2;
	unsigned short m = V(LN_MASK);

	if (x1 >= 0 && x2 >= 0 && y1 >= 0 && y2 >= 0 && x1 < sw && x2 < sw && y1 < sh && y2 < sh) {
		fb->line(fb_wk, x1, y1, x2, y2, m, la_colour(), 0, mode());
		return;
	}
	dx = x2 > x1 ? x2 - x1 : x1 - x2;
	dy = y2 > y1 ? y2 - y1 : y1 - y2;
	sx = x2 > x1 ? 1 : -1;
	sy = y2 > y1 ? 1 : -1;
	err = dx - dy;
	for (;;) {
		pixel(x1, y1, la_colour(), 0, mode(), m & 0x8000);
		m = m << 1 | m >> 15;
		if (x1 == x2 && y1 == y2)
			break;
		e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x1 += sx; }
		if (e2 < dx) { err += dx; y1 += sy; }
	}
}

/* one scan line of a polygon: spans between edge crossings, even-odd */
static void polygon(void)
{
	short *p = VP(PTSIN);
	long n = VP(CONTRL)[1], y = V(Y1), xs[64], c = 0, i, j, t;
	long ax, ay, bx, by, xmin = 0, xmax = sw - 1;

	for (i = 0; i < n && c < 64; i++) {
		ax = p[2 * i]; ay = p[2 * i + 1];
		bx = p[2 * ((i + 1) % n)]; by = p[2 * ((i + 1) % n) + 1];
		if (ay == by || (y < ay && y < by) || (y >= ay && y >= by))
			continue;
		xs[c++] = ax + (y - ay) * (bx - ax) / (by - ay);
	}
	for (i = 1; i < c; i++)
		for (j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
			t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t;
		}
	if (V(CLIP)) {
		xmin = V(XMINCL);
		xmax = V(XMAXCL);
	}
	for (i = 0; i + 1 < c; i += 2)
		rect(xs[i] < xmin ? xmin : xs[i], y, xs[i + 1] > xmax ? xmax : xs[i + 1], y, 0);
}

/*
 * A raster: the screen (one byte per pixel) or a planar form, its
 * planes nxpl bytes apart, 16-pixel words nxwd apart.
 */
struct form {
	char *base;
	long nxwd, nxln, nxpl, chunky;
	long x0, y0;		/* on the screen: where base is */
};

static void onscreen(struct form *f)
{
	long o, k;
	char *a;

	/* on the program's own screen: the same place on this one */
	for (k = 0; k < 2; k++) {
		a = alias[k];
		if (a && f->base >= a && f->nxln > 0 && f->nxwd > 0 && f->base - a < f->nxln * sh) {
			o = f->base - a;
			f->base = (char *)(scr + o / f->nxln * wrap + o % f->nxln / f->nxwd * 16);
			break;
		}
	}
	o = (Pix *)f->base - scr;

	f->chunky = o >= 0 && o < wrap * sh;
	if (f->chunky) {
		f->nxln = wrap;
		f->x0 = o % wrap;
		f->y0 = o / wrap;
	}
}

static long fget(struct form *f, long x, long y, long planes)
{
	unsigned short *w;
	long v = 0, p;

	if (f->chunky)
		return ((Pix *)f->base)[y * wrap + x];
	w = (unsigned short *)(f->base + y * f->nxln + (x >> 4) * f->nxwd);
	for (p = 0; p < planes; p++, w = (unsigned short *)((char *)w + f->nxpl))
		v |= (*w >> (15 - (x & 15)) & 1) << p;
	return v;
}

static void fput(struct form *f, long x, long y, long planes, long v)
{
	unsigned short *w, b = 0x8000 >> (x & 15);
	long p;

	if (f->chunky) {
		((Pix *)f->base)[y * wrap + x] = v;
		return;
	}
	w = (unsigned short *)(f->base + y * f->nxln + (x >> 4) * f->nxwd);
	for (p = 0; p < planes; p++, w = (unsigned short *)((char *)w + f->nxpl))
		*w = v >> p & 1 ? *w | b : *w & ~b;
}

/* keep a screen rectangle on the screen; the other side moves with it */
static int clipon(struct form *f, long *x, long *y, long *ox, long *oy, long *w, long *h)
{
	long t;

	if (!f->chunky)
		return 1;
	if ((t = -(f->x0 + *x)) > 0) { *x += t; *ox += t; *w -= t; }
	if ((t = -(f->y0 + *y)) > 0) { *y += t; *oy += t; *h -= t; }
	if ((t = f->x0 + *x + *w - sw) > 0) *w -= t;
	if ((t = f->y0 + *y + *h - sh) > 0) *h -= t;
	return *w > 0 && *h > 0;
}

struct blt {
	long planes, fg, bg;
	unsigned char ops[4];
	unsigned short *pat;
	long pnxln, pnxpl, pmask;
};

/* per plane: the logic op its fg and bg bits pick */
static void la_blit(struct form *s, long sx, long sy, struct form *d, long dx, long dy,
    long w, long h, struct blt *b)
{
	unsigned char m[4] = { 0, 0, 0, 0 };
	long i, j, k, p, one = -1, sv, dv, r, pv;
	long planes = b->planes > 8 ? 8 : b->planes;

	if (!clipon(d, &dx, &dy, &sx, &sy, &w, &h) || !clipon(s, &sx, &sy, &dx, &dy, &w, &h))
		return;
	/* one plane onto the screen: the same bits in all of its planes */
	if (planes == 1 && d->chunky) {
		planes = 8;
		b->fg = b->fg & 1 ? 0xff : 0;
		b->bg = b->bg & 1 ? 0xff : 0;
	}
	for (p = 0; p < planes; p++)
		m[(b->fg >> p & 1) << 1 | (b->bg >> p & 1)] |= 1 << p;
	for (k = 0; k < 4; k++)
		if (m[k] == 0xff)
			one = b->ops[k] & 15;
	if (one >= 0 && s->chunky && d->chunky && !b->pat) {
		fb->blit((Pix *)s->base + sy * wrap + sx, wrap,
		    (Pix *)d->base + dy * wrap + dx, wrap, w, h, one);
		return;
	}
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			sv = fget(s, sx + i, sy + j, planes);
			/* a one-plane source stands for every plane */
			if (!s->chunky && (b->planes == 1 || !s->nxpl))
				sv = sv & 1 ? 0xff : 0;
			if (b->pat) {
				for (pv = 0, p = 0; p < planes; p++)
					pv |= (b->pat[(((dy + j) & b->pmask) * b->pnxln + p * b->pnxpl) / 2]
					    >> (15 - ((dx + i) & 15)) & 1) << p;
				sv &= b->pnxpl ? pv : pv & 1 ? 0xff : 0;
			}
			dv = fget(d, dx + i, dy + j, d->chunky ? 8 : planes);
			for (r = dv & ~((1L << planes) - 1), k = 0; k < 4; k++)
				if (m[k])
					r |= fb_op(b->ops[k] & 15, sv, dv) & m[k];
			fput(d, dx + i, dy + j, planes, r);
		}
}

/* $A007: the parameter block at a6 */
static void bitblt(short *f)
{
	struct form s, d;
	struct blt b;
	long k;

	s.base = *(char **)(f + 9);
	s.nxwd = f[11]; s.nxln = f[12]; s.nxpl = f[13];
	d.base = *(char **)(f + 16);
	d.nxwd = f[18]; d.nxln = f[19]; d.nxpl = f[20];
	onscreen(&s);
	onscreen(&d);
	b.planes = f[2];
	b.fg = f[3];
	b.bg = f[4];
	for (k = 0; k < 4; k++)
		b.ops[k] = ((unsigned char *)f)[10 + k];
	b.pat = *(unsigned short **)(f + 21);
	b.pnxln = f[23]; b.pnxpl = f[24]; b.pmask = f[25];
	la_blit(&s, f[7], f[8], &d, f[14], f[15], f[0], f[1], &b);
}

/* an MFDB as a form; address 0, the screen's or the program's own is the screen */
static void mfdb(MFDB *m, struct form *f)
{
	f->base = m->address && !la_alias(m->address) ? (char *)m->address : (char *)scr;
	f->nxln = 0;
	onscreen(f);
	if (f->chunky)
		return;
	f->nxln = m->wdwidth * 2;
	if (m->bitplanes == 8 && !m->standard) {
		f->chunky = 1;
		f->x0 = f->y0 = 0;
		f->nxln = m->wdwidth * 2 * 8;
		return;
	}
	if (m->standard || m->bitplanes == 1) {
		f->nxwd = 2;
		f->nxpl = (long)m->wdwidth * 2 * m->height;
	} else {
		f->nxwd = 2 * m->bitplanes;
		f->nxpl = 2;
		f->nxln *= m->bitplanes;
	}
}

/* $A00E: vro_cpyfm, or vrt_cpyfm when COPYTRAN is set */
static void raster(void)
{
	short *c = VP(CONTRL), *in = VP(INTIN), *pt = VP(PTSIN);
	MFDB *sm = *(MFDB **)(c + 7), *dm = *(MFDB **)(c + 9);
	struct form s, d;
	struct blt b;
	long sx = pt[0], sy = pt[1], dx = pt[4], dy = pt[5];
	long w = pt[2] - pt[0] + 1, h = pt[3] - pt[1] + 1, t, k;

	mfdb(sm, &s);
	mfdb(dm, &d);
	if (V(COPYTRAN)) {
		if (!clipon(&d, &dx, &dy, &sx, &sy, &w, &h) || s.chunky || !d.chunky)
			return;
		t = sm->wdwidth * 2;
		fb->expand((unsigned short *)(s.base + sy * t), t, sx,
		    (Pix *)d.base + dy * wrap + dx, wrap, w, h,
		    fb_pen(in[1]), fb_pen(in[2]), in[0] < 1 || in[0] > 4 ? 1 : in[0]);
		return;
	}
	b.planes = sm->bitplanes > 8 || s.chunky ? 8 : sm->bitplanes;
	b.fg = 0xff;
	b.bg = 0;
	for (k = 0; k < 4; k++)
		b.ops[k] = in[0];
	b.pat = 0;
	la_blit(&s, sx, sy, &d, dx, dy, w, h, &b);
}

/* $A008: one character from the font form, with bold, light and italic */
static void textblt(void)
{
	unsigned char *f = (unsigned char *)VL(FBASE);
	long fw = V(FWIDTH), sx = V(SOURCEX), sy = V(SOURCEY);
	long dx = V(DESTX), dy = V(DESTY), w = V(DELX), h = V(DELY);
	long st = V(STYLE), wt = st & 1 ? V(WEIGHT) : 0, fg = V(TEXTFG) & 0xff, bg = V(TEXTBG) & 0xff;
	unsigned short light = V(LITEMASK), skew = V(SKEWMASK);
	long m = V(WMODE), i, j, k, x, y, off = 0, xmin = 0, ymin = 0, xmax = sw - 1, ymax = sh - 1;
	unsigned long row[9], bits;
	Pix *d;

	if (w + wt > 256)
		return;
	if (V(CLIP)) {
		xmin = V(XMINCL); ymin = V(YMINCL);
		xmax = V(XMAXCL); ymax = V(YMAXCL);
		if (xmin < 0) xmin = 0;
		if (ymin < 0) ymin = 0;
		if (xmax >= sw) xmax = sw - 1;
		if (ymax >= sh) ymax = sh - 1;
	}
	if (!st && m < 4 && dx >= xmin && dy >= ymin && dx + w - 1 <= xmax && dy + h - 1 <= ymax &&
	    !((long)f & 1) && !(fw & 1)) {
		fb->expand((unsigned short *)(f + sy * fw), fw, sx, scr + dy * wrap + dx, wrap,
		    w, h, fg, bg, m + 1);
		return;
	}
	if (st & 4)		/* italic leans right from the bottom */
		for (j = 0; j < h; j++)
			if ((unsigned short)(skew << (j & 15) | skew >> (16 - (j & 15))) & 0x8000)
				off++;
	for (j = 0; j < h; j++) {
		y = dy + j;
		for (k = 0; k < 9; k++)
			row[k] = 0;
		for (i = 0; i < w; i++)
			if (f[(sy + j) * fw + ((sx + i) >> 3)] & 0x80 >> ((sx + i) & 7))
				row[i >> 5] |= 0x80000000UL >> (i & 31);
		for (k = 0; k < wt; k++)	/* bold: smear right */
			for (i = 8; i >= 0; i--)
				row[i] |= row[i] >> 1 | (i ? row[i - 1] << 31 : 0);
		if (st & 2) {
			light = light << 1 | light >> 15;
			if (!(light & 0x8000))
				for (k = 0; k < 9; k++)
					row[k] = 0;
		}
		if (st & 4 && (unsigned short)(skew << (j & 15) | skew >> (16 - (j & 15))) & 0x8000)
			off--;
		if (y < ymin || y > ymax)
			continue;
		for (i = 0; i < w + wt; i++) {
			x = dx + i + off;
			if (x < xmin || x > xmax)
				continue;
			bits = row[i >> 5] & 0x80000000UL >> (i & 31);
			d = scr + y * wrap + x;
			if (m < 4)
				pixel(x, y, fg, bg, m + 1, bits != 0);
			else
				*d = fb_op(m - 4, bits ? fg : bg, *d);
		}
	}
}

static int inside(long x, long y, long c, long same)
{
	long o = y * sw + x;

	return !(seen[o >> 3] & 1 << (o & 7)) && (scr[y * wrap + x] == c) == same;
}

/* $A00F: from the point at PTSIN, the pixels of its colour or up to a border */
static void seedfill(void)
{
	short *in = VP(INTIN), *pt = VP(PTSIN);
	long (*stop)(void) = (long (*)(void))VL(SEEDABORT);
	long x = pt[0], y = pt[1], c, same, l, r, i, k, n = 0, xmin = 0, ymin = 0, xmax = sw - 1, ymax = sh - 1;
	static short st[2 * 4096];
	unsigned short pat[16];

	if (!seen)
		return;
	if (V(CLIP)) {
		if (V(XMINCL) > xmin) xmin = V(XMINCL);
		if (V(YMINCL) > ymin) ymin = V(YMINCL);
		if (V(XMAXCL) < xmax) xmax = V(XMAXCL);
		if (V(YMAXCL) < ymax) ymax = V(YMAXCL);
	}
	if (x < xmin || x > xmax || y < ymin || y > ymax)
		return;
	same = in[0] < 0;
	c = same ? scr[y * wrap + x] : fb_pen(in[0]);
	for (i = 0; i < (sw * sh + 7) / 8; i++)
		seen[i] = 0;
	solid(pat);
	st[n++] = x;
	st[n++] = y;
	while (n) {
		y = st[--n];
		x = st[--n];
		if (!inside(x, y, c, same))
			continue;
		if (stop && stop())
			break;
		for (l = x; l > xmin && inside(l - 1, y, c, same); l--)
			;
		for (r = x; r < xmax && inside(r + 1, y, c, same); r++)
			;
		for (i = l; i <= r; i++)
			seen[(y * sw + i) >> 3] |= 1 << ((y * sw + i) & 7);
		/* one seed per run of fillable pixels above and below */
		for (k = -1; k <= 1; k += 2) {
			if (y + k < ymin || y + k > ymax)
				continue;
			for (i = l; i <= r && n < 2 * 4096 - 2; i++)
				if (inside(i, y + k, c, same) && (i == l || !inside(i - 1, y + k, c, same))) {
					st[n++] = i;
					st[n++] = y + k;
				}
		}
		fb->fill(fb_wk, l, y, r - l + 1, 1, pat, la_colour(), 0, mode());
	}
}

/* sprites: a 16x16 shape, what it covers saved in the caller's buffer */
struct save {
	short h, w;
	Pix *at;
	short valid;
	Pix pix[256];
};

static void unsprite(struct save *s)
{
	Pix *d = s->at, *p = s->pix;
	long i, j;

	if (!s->valid)
		return;
	for (j = 0; j < s->h; j++, d += wrap)
		for (i = 0; i < s->w; i++)
			d[i] = *p++;
	s->valid = 0;
}

static void sprite(short *def, long x, long y, struct save *s)
{
	long i, j, i0 = 0, j0 = 0, w = 16, h = 16, fg = def[4] & 0xff, bg = def[3] & 0xff;
	unsigned short mk, dt;
	Pix *d, *p = s->pix;

	x -= def[0];
	y -= def[1];
	if (x < 0) { i0 = -x; w += x; x = 0; }
	if (y < 0) { j0 = -y; h += y; y = 0; }
	if (x + w > sw) w = sw - x;
	if (y + h > sh) h = sh - y;
	s->valid = 0;
	if (w <= 0 || h <= 0)
		return;
	s->at = scr + y * wrap + x;
	s->w = w;
	s->h = h;
	s->valid = 1;
	for (d = s->at, j = 0; j < h; j++, d += wrap) {
		mk = def[5 + 2 * (j + j0)] << i0;
		dt = def[6 + 2 * (j + j0)] << i0;
		for (i = 0; i < w; i++, mk <<= 1, dt <<= 1) {
			*p++ = d[i];
			if (def[2] == -1) {	/* XOR form */
				if (mk & 0x8000)
					d[i] = bg;
				if (dt & 0x8000)
					d[i] ^= fg;
			} else if (dt & 0x8000)
				d[i] = fg;
			else if (mk & 0x8000)
				d[i] = bg;
		}
	}
}

static void la_mouse(long op)
{
	Workstation *wk = fb_wk;
	short *in = VP(INTIN);
	long i;

	switch (op) {
	case 9:
		if (in[0] == 0)
			V(M_HID_CNT) = 0;
		else if (V(M_HID_CNT) > 0)
			V(M_HID_CNT)--;
		else
			return;
		if (V(M_HID_CNT) == 0) {
			*(la + CUR_FLAG) = 0;
			c_mouse_draw(wk, V(GCURX), V(GCURY), (Mouse *)3);
		}
		break;
	case 10:
		if (++V(M_HID_CNT) == 1)
			c_mouse_draw(wk, V(GCURX), V(GCURY), (Mouse *)2);
		break;
	case 11:
		wk->mouse.hotspot.x = in[0];
		wk->mouse.hotspot.y = in[1];
		wk->mouse.colour.background = in[3];
		wk->mouse.colour.foreground = in[4];
		for (i = 0; i < 16; i++) {
			wk->mouse.mask[i] = in[5 + i];
			wk->mouse.data[i] = in[21 + i];
		}
		c_mouse_draw(wk, V(GCURX), V(GCURY), &wk->mouse);
		if (V(M_HID_CNT) == 0)
			c_mouse_draw(wk, V(GCURX), V(GCURY), (Mouse *)0);
		break;
	}
}

long CDECL la_call(long op, long *r)
{
	short *p;

	/* a direct call runs in user mode: the system variable stays */
	la_screen(op < 16);
	switch (op & 15) {
	case 0:
		r[D0] = r[A0] = (long)la;
		r[A1] = la_fonts;
		r[A2] = (long)la_vecs;
		break;
	case 1:
		p = VP(PTSIN);
		if (p[0] >= 0 && p[1] >= 0 && p[0] < sw && p[1] < sh)
			scr[p[1] * wrap + p[0]] = VP(INTIN)[0];
		break;
	case 2:
		p = VP(PTSIN);
		r[D0] = p[0] >= 0 && p[1] >= 0 && p[0] < sw && p[1] < sh ? scr[p[1] * wrap + p[0]] : 0;
		break;
	case 3:
		la_line();
		break;
	case 4:
		rect(V(X1), V(Y1), V(X2), V(Y1), 0);
		break;
	case 5:
		rect(V(X1), V(Y1), V(X2), V(Y2), V(CLIP));
		break;
	case 6:
		polygon();
		break;
	case 7:
		bitblt((short *)r[A6]);
		break;
	case 8:
		textblt();
		break;
	case 9: case 10: case 11:
		la_mouse(op);
		break;
	case 12:
		unsprite((struct save *)r[A2]);
		break;
	case 13:
		sprite((short *)r[A0], (short)r[D0], (short)r[D1], (struct save *)r[A2]);
		break;
	case 14:
		raster();
		break;
	case 15:
		seedfill();
		break;
	}
	return 0;
}
