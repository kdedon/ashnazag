/*
 * prn.c -- printing through the printer drivers, as GDI does it: Wabi's
 * own (WHPPCL5A.DRV for the LaserJets, WBIEPSON.DRV for the Epsons) or
 * any Windows 3.1 raster printer driver the user has.
 *
 * CreateDC("WHPPCL5A", "HP Laserjet III (Wabi)", "LPT1:", devmode) loads
 * the driver and has it fill its GDIINFO and its PDEVICE (Enable);
 * GetDeviceCaps answers from the GDIINFO.  A printer DC draws on nothing
 * of its own: what is drawn on a page is recorded as a metafile
 * (metafile.c), the DC kept up to date meanwhile for the program's
 * questions (text metrics at the printer's resolution).  At the end of a
 * page the driver gives the bands it wants (Control, NEXTBAND) and each
 * is drawn by playing the page into a monochrome bitmap of the band at
 * the printer's resolution, which goes to the driver's BitBlt; the
 * driver makes its printer's language of it and writes that through
 * GDI's spooler (OpenJob, WriteSpool, CloseJob), which hands the job to
 * the port's command: WABI.INI's Printers.command_lptN as Wabi had it,
 * or lp.
 *
 * GDI's brute-force functions the drivers draw their band bitmaps with
 * (dmBitBlt and the rest) are here, for monochrome bitmaps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "w16.h"
#include "win.h"

extern u32 ualloc(), ulin();
extern void ufree();
extern apifn api_fn();

/* the driver's entries, by ordinal */
#define	E_BITBLT	1
#define	E_COLORINFO	2
#define	E_CONTROL	3
#define	E_DISABLE	4
#define	E_ENABLE	5
#define	E_ENUMDFONTS	6
#define	E_OUTPUT	8
#define	E_REALIZEOBJECT	10
#define	E_EXTDEVICEMODE	90

#define	NEWFRAME	1
#define	ABORTDOC	2
#define	NEXTBAND	3
#define	QUERYESCSUPPORT	8
#define	SETABORTPROC	9
#define	STARTDOC	10
#define	ENDDOC		11

#define	RC_BANDING	0x0002

struct prn {
	struct module *m;
	u16	pdev;		/* its PDEVICE, a block of its own */
	u16	info;		/* its GDIINFO */
	u16	strs;		/* the device and port names, and a DRAWMODE */
	u32	devfp, portfp, dmfp;
	u32	devmode;	/* the program's, as given */
	int	ic;		/* an information context: no printing */
	int	indoc, inpage, aborted;
	u32	abortproc;
	u16	hdc;
	char	port[64];
};

/* ---- calling the driver ---- */

static u32
entry(p, ord)
	struct prn *p;
	int ord;
{
	return mod_proc(p->m, ord, (char *)0);
}

static int
caps(p, idx)
	struct prn *p;
	int idx;
{
	return idx >= 0 && idx < 110 ? (short)GW(sel_base(p->info) + idx) : 0;
}

/* Control(lpPDevice, function, lpInData, lpOutData) */
static int
control(p, fn, in, out)
	struct prn *p;
	int fn;
	u32 in, out;
{
	u32 e = entry(p, E_CONTROL);

	if (!e)
		return 0;
	cb_begin();
	cb_push32(FP(p->pdev, 0));
	cb_push16(fn);
	cb_push32(in);
	cb_push32(out);
	return (short)(cb_call(e, 0) & 0xffff);
}

/* a physical colour of the driver's (ColorInfo) */
static u32
physcolor(p, rgb)
	struct prn *p;
	u32 rgb;
{
	u32 e = entry(p, E_COLORINFO), b = ualloc(4), r;

	if (!e)
		return rgb;
	cb_begin();
	cb_push32(FP(p->pdev, 0));
	cb_push32(rgb);
	cb_push32(b);
	cb_call(e, 0);
	r = GL(ulin(b));
	ufree(b);
	return r;
}

struct prn *
prn_of(h)
	u32 h;
{
	struct dc *dc = dc_get(h);

	return dc ? (struct prn *)dc->prn : 0;
}

/* a string into the block, at off: its far pointer */
static u32
putstr(sel, off, s)
	u16 sel;
	int off;
	char *s;
{
	strcpy((char *)M + sel_base(sel) + off, s ? s : "");
	return FP(sel, off);
}

/*
 * CreateDC/CreateIC for a printer: the driver's module (driver.DRV),
 * its GDIINFO, then its PDEVICE; 0 if it will not.
 */
u16
prn_create(drv, device, port, devmode, ic)
	char *drv, *device, *port;
	u32 devmode;
	int ic;
{
	char name[80];
	struct module *m;
	struct prn *p;
	struct dc *dc;
	u32 e, r;
	u16 h;
	int err, size;

	if (!drv || !*drv || strlen(drv) > 60)
		return 0;
	sprintf(name, strchr(drv, '.') ? "%s" : "%s.DRV", drv);
	if ((m = mod_load(name, &err)) == 0)
		return 0;
	{
		extern void initdeps();

		initdeps(m, 0);
	}
	p = (struct prn *)calloc(1, sizeof *p);
	p->m = m;
	p->ic = ic;
	p->info = g_alloc(GMEM_ZEROINIT, (u32)128, 0);
	p->strs = g_alloc(GMEM_ZEROINIT, (u32)512, 0);
	strncpy(p->port, port ? port : "", sizeof p->port - 1);
	p->devfp = putstr(p->strs, 0, device);
	p->portfp = putstr(p->strs, 128, port);
	p->dmfp = FP(p->strs, 256);
	p->devmode = devmode;
	if ((e = entry(p, E_ENABLE)) == 0)
		goto bad;
	/* Enable(lpDevInfo, style, lpDeviceType, lpOutputFile, lpInitData): first the GDIINFO (InquireInfo) */
	cb_begin();
	cb_push32(FP(p->info, 0));
	cb_push16(ic ? 0x8001 : 1);
	cb_push32(p->devfp);
	cb_push32(p->portfp);
	cb_push32(devmode);
	r = cb_call(e, 0) & 0xffff;
	size = GW(sel_base(p->info) + 26);	/* dpDEVICEsize */
	if (!r || size <= 0)
		goto bad;
	p->pdev = g_alloc(GMEM_ZEROINIT, (u32)size + 16, 0);
	cb_begin();
	cb_push32(FP(p->pdev, 0));
	cb_push16(ic ? 0x8000 : 0);
	cb_push32(p->devfp);
	cb_push32(p->portfp);
	cb_push32(devmode);
	r = cb_call(e, 0) & 0xffff;
	if (!r)
		goto bad;
	h = dc_new(DCK_PRINTER);
	dc = dc_get(h);
	p->hdc = h;
	dc->prn = (void *)p;
	dc->prnres = caps(p, 90);
	/* nothing to draw on: a pixel, of the device's kind */
	{
		static struct surf one;
		static u8 pix[1];

		one.w = one.h = one.rowb = 1;
		one.pix = pix;
		one.mono = caps(p, 12) * caps(p, 14) <= 1;
		dc->s = &one;
		rgn_init(&dc->vis);
	}
	if (w16_debug)
		w16_log("startwin: printer %s (%s) on %s: %dx%d at %d dpi, %d bits, raster %04x\n", device ? device : "",
		    name, port ? port : "", caps(p, 8), caps(p, 10), caps(p, 88), caps(p, 12) * caps(p, 14), caps(p, 38));
	if (w16_debug > 1) {
		int i;

		w16_log("startwin: printer %dx%d mm, %dx%d dpi, mapping modes", caps(p, 4), caps(p, 6), caps(p, 88),
		    caps(p, 90));
		for (i = 48; i < 88; i += 8)
			w16_log(" %d,%d:%d,%d", caps(p, i), caps(p, i + 2), caps(p, i + 4), caps(p, i + 6));
		w16_log("\n");
	}
	return h;
bad:
	if (w16_debug)
		w16_log("startwin: printer driver %s will not enable\n", name);
	if (p->pdev)
		g_free(p->pdev);
	g_free(p->info);
	g_free(p->strs);
	mod_free(m);
	free(p);
	return 0;
}

/* GetDeviceCaps: the GDIINFO's word */
int
prn_caps(dc, idx)
	struct dc *dc;
	int idx;
{
	return caps((struct prn *)(dc->prn ? dc->prn : dc->prnof), idx);
}

/* DeleteDC: Disable, the driver let go */
void
prn_delete(dc)
	struct dc *dc;
{
	struct prn *p = (struct prn *)dc->prn;
	u32 e;

	if (!p)
		return;
	if (p->indoc)
		control(p, ABORTDOC, (u32)0, (u32)0);
	if ((e = entry(p, E_DISABLE)) != 0) {
		cb_begin();
		cb_push32(FP(p->pdev, 0));
		cb_call(e, 0);
	}
	g_free(p->pdev);
	g_free(p->info);
	g_free(p->strs);
	mod_free(p->m);
	free(p);
	dc->prn = 0;
}

/* ---- the job ---- */

/* the program's abort procedure, between bands: 0 stops */
static int
stillgoing(p)
	struct prn *p;
{
	if (p->aborted)
		return 0;
	if (!p->abortproc)
		return 1;
	cb_begin();
	cb_push16(p->hdc);
	cb_push16(0);
	if (!(cb_call(p->abortproc, 0) & 0xffff))
		p->aborted = 1;
	return !p->aborted;
}

static int
startdoc(p, name)
	struct prn *p;
	char *name;
{
	u32 hp = FP(p->strs, 300), np = putstr(p->strs, 320, name ? name : "");
	int r;

	if (p->ic)
		return -1;
	/* the driver keeps the DC for its job (SETABORTPROC brings it), then STARTDOC opens the job */
	PW(sel_base(p->strs) + 300, p->hdc);
	control(p, SETABORTPROC, hp, (u32)0);
	r = control(p, STARTDOC, np, (u32)0);
	if (r <= 0)
		return r ? r : -1;
	p->indoc = 1;
	p->aborted = 0;
	return r;
}

/* the page drawn band by band, given to the driver */
static int endpage();

static int
enddoc(p)
	struct prn *p;
{
	struct dc *dc = dc_get(p->hdc);

	if (!p->indoc)
		return -1;
	if (p->inpage && dc && dc->meta)
		endpage(p);
	p->indoc = 0;
	return control(p, p->aborted ? ABORTDOC : ENDDOC, (u32)0, (u32)0);
}

/* a page starts: its recording, with the DC's state as it is */
static void
startpage(p)
	struct prn *p;
{
	extern void meta_beginpage();
	struct dc *dc = dc_get(p->hdc);

	if (!dc || p->inpage)
		return;
	meta_beginpage(dc);
	p->inpage = 1;
}

/* a band of the page: played into a monochrome bitmap of it, given to the driver's BitBlt */
static void
band(p, hmf, l, t, r, b)
	struct prn *p;
	u32 hmf;
	int l, t, r, b;
{
	u32 a[13], bm, mdc, old, e = entry(p, E_BITBLT), pb, bits;
	struct dc *md;
	struct surf *s;
	int w = r - l, h = b - t, y, x, rows, wb, y0;
	u16 bsel;
	u8 *row, *q;

	if (w <= 0 || h <= 0 || !e)
		return;
	if (w16_debug > 1)
		w16_log("startwin: printer band %d,%d-%d,%d\n", l, t, r, b);
	a[0] = w;
	a[1] = h;
	a[2] = 1;
	a[3] = 1;
	a[4] = 0;
	bm = api_fn("GDI", "CreateBitmap")(a);
	a[0] = 0;
	mdc = api_fn("GDI", "CreateCompatibleDC")(a);
	if (!bm || !mdc)
		return;
	a[0] = mdc;
	a[1] = bm;
	old = api_fn("GDI", "SelectObject")(a);
	a[0] = mdc; a[1] = 0; a[2] = 0; a[3] = w; a[4] = h; a[5] = 0xff0062L;	/* WHITENESS */
	api_fn("GDI", "PatBlt")(a);
	/* the page's coordinates are the printer's: the band's origin moved to its place */
	md = dc_get(mdc);
	md->ox = -l;
	md->oy = -t;
	md->effok = 0;
	md->prnres = caps(p, 90);
	md->prnof = (void *)p;
	a[0] = mdc;
	a[1] = hmf;
	api_fn("GDI", "PlayMetaFile")(a);
	s = md->s;
	/* to the driver in pieces of under 64K, as a device bitmap: 1 bit a pixel, 1 white */
	wb = ((w + 15) / 16) * 2;
	rows = 60000 / wb;
	if (rows < 1)
		rows = 1;
	bsel = g_alloc(GMEM_ZEROINIT, (u32)wb * (rows < h ? rows : h) + 64, 0);
	pb = FP(bsel, 0);
	bits = FP(bsel, 32);
	for (y0 = 0; y0 < h && stillgoing(p); y0 += rows) {
		int n = h - y0 < rows ? h - y0 : rows;
		u32 lb = sel_base(bsel);

		PW(lb, 0);			/* bmType */
		PW(lb + 2, w);
		PW(lb + 4, n);
		PW(lb + 6, wb);
		PB(lb + 8, 1);
		PB(lb + 9, 1);
		PL(lb + 10, bits);
		PL(lb + 14, (u32)wb * n);
		PL(lb + 18, 0);
		PW(lb + 22, 0);
		PW(lb + 24, n);
		PW(lb + 26, 0);
		for (y = 0; y < n; y++) {
			row = s->pix + (y0 + y) * s->rowb;
			q = M + lb + 32 + y * wb;
			memset(q, 0xff, wb);
			for (x = 0; x < w; x++)
				if (s->mono ? !row[x] : row[x] != 15 && row[x] != 255)
					q[x >> 3] &= ~(0x80 >> (x & 7));
		}
		/* BitBlt(lpDestDev, x, y, lpSrcDev, sx, sy, w, h, rop, lpPBrush, lpDrawMode) */
		cb_begin();
		cb_push32(FP(p->pdev, 0));
		cb_push16(l);
		cb_push16(t + y0);
		cb_push32(pb);
		cb_push16(0);
		cb_push16(0);
		cb_push16(w);
		cb_push16(n);
		cb_push32(0xcc0020L);		/* SRCCOPY */
		cb_push32(0);
		cb_push32(p->dmfp);
		cb_call(e, 0);
	}
	g_free(bsel);
	a[0] = mdc;
	a[1] = old;
	api_fn("GDI", "SelectObject")(a);
	api_fn("GDI", "DeleteDC")(a);
	a[0] = bm;
	api_fn("GDI", "DeleteObject")(a);
}

/* the DRAWMODE the bands go with: copy, opaque, white on black as the driver has them */
static void
drawmode(p)
	struct prn *p;
{
	u32 d = sel_base(p->strs) + 256;

	memset(M + d, 0, 40);
	PW(d, 13);			/* R2_COPYPEN */
	PW(d + 2, 2);			/* OPAQUE */
	PL(d + 4, physcolor(p, RGB(255, 255, 255)));
	PL(d + 8, physcolor(p, 0));
	PL(d + 24, RGB(255, 255, 255));
	PL(d + 28, 0);
}

static int
endpage(p)
	struct prn *p;
{
	extern u32 meta_endpage();
	struct dc *dc = dc_get(p->hdc);
	u32 hmf, rp, rl;
	int r = 1, n = 0;

	if (!dc || !p->inpage)
		return -1;
	p->inpage = 0;
	hmf = meta_endpage(dc);
	if (!hmf)
		return -1;
	drawmode(p);
	if (caps(p, 38) & RC_BANDING) {
		/* NEXTBAND: the band's rectangle out, a POINT in for the driver's scaling of it */
		rp = ualloc(12);
		rl = ulin(rp);
		for (;;) {
			memset(M + rl, 0, 12);
			if ((r = control(p, NEXTBAND, FP(FPSEL(rp), FPOFF(rp) + 8), rp)) <= 0 || !stillgoing(p))
				break;
			if (w16_debug > 1)
				w16_log("startwin: printer NEXTBAND %d: %d,%d-%d,%d scale %d,%d\n", r, (short)GW(rl),
				    (short)GW(rl + 2), (short)GW(rl + 4), (short)GW(rl + 6), (short)GW(rl + 8), (short)GW(rl + 10));
			if ((short)GW(rl) >= (short)GW(rl + 4) || (short)GW(rl + 2) >= (short)GW(rl + 6))
				break;		/* empty: the page is out */
			band(p, hmf, (short)GW(rl), (short)GW(rl + 2), (short)GW(rl + 4), (short)GW(rl + 6));
			if (++n > 10000)
				break;
		}
		ufree(rp);
	} else {
		band(p, hmf, 0, 0, caps(p, 8), caps(p, 10));
		r = control(p, NEWFRAME, (u32)0, (u32)0);
	}
	g_free(hmf);
	if (p->aborted)
		return -1;
	return r > 0 ? 1 : r;
}

/* Escape on a printer DC: the job's own here, the rest to the driver's Control */
u32
prn_escape(dc, n, cb, in, out)
	struct dc *dc;
	int n, cb;
	u32 in, out;
{
	struct prn *p = (struct prn *)dc->prn;

	(void)cb;
	switch (n) {
	case QUERYESCSUPPORT:
		{
			u32 l = lin(FPSEL(in), FPOFF(in));
			int q = l ? GW(l) : 0;

			if (q == NEWFRAME || q == ABORTDOC || q == NEXTBAND || q == SETABORTPROC || q == STARTDOC ||
			    q == ENDDOC || q == QUERYESCSUPPORT)
				return 1;
			return control(p, n, in, out);
		}
	case SETABORTPROC:
		p->abortproc = in;
		return 1;
	case STARTDOC:
		{
			char name[128];
			u32 l = lin(FPSEL(in), FPOFF(in));

			name[0] = 0;
			if (l) {
				int k = cb > 0 && cb < (int)sizeof name ? cb : (int)sizeof name - 1;

				strncpy(name, (char *)M + l, k);
				name[k] = 0;
			}
			if (startdoc(p, name) <= 0)
				return (u32)-1;
			startpage(p);
			return 1;
		}
	case NEWFRAME:
		if (!p->indoc)
			return (u32)-1;
		if (endpage(p) <= 0)
			return (u32)-1;
		startpage(p);
		return 1;
	case NEXTBAND:
		/* the program bands: the whole page its one band, then the page goes out (ours are the driver's bands) */
		{
			u32 l = lin(FPSEL(out), FPOFF(out));

			if (!p->indoc || !l)
				return (u32)-1;
			if (!p->inpage) {
				/* the band asked after the page went: the next page */
				startpage(p);
			}
			if (dc->prnband == 0) {
				PW(l, 0);
				PW(l + 2, 0);
				PW(l + 4, caps(p, 8));
				PW(l + 6, caps(p, 10));
				dc->prnband = 1;
				return 1;
			}
			dc->prnband = 0;
			memset(M + l, 0, 8);
			if (endpage(p) <= 0)
				return (u32)-1;
			startpage(p);
			return 1;
		}
	case ENDDOC:
		return enddoc(p) < 0 ? (u32)-1 : 1;
	case ABORTDOC:
		p->aborted = 1;
		if (p->indoc) {
			p->inpage = 0;
			{
				extern u32 meta_endpage();
				u32 h = meta_endpage(dc);

				if (h)
					g_free(h);
			}
			p->indoc = 0;
			control(p, ABORTDOC, (u32)0, (u32)0);
		}
		return 1;
	}
	return control(p, n, in, out);
}

/* StartDoc(hdc, DOCINFO far *): size, the document's name, the output */
static u32
g_StartDoc(a)
	u32 *a;
{
	struct prn *p = prn_of(a[0]);
	u32 d = lin(FPSEL(a[1]), FPOFF(a[1]));
	char *name = d && GL(d + 2) ? gptr(GL(d + 2)) : "";

	if (!p)
		return (u32)-1;
	if (startdoc(p, name) <= 0)
		return (u32)-1;
	return 1;
}

static u32
g_EndDoc(a)
	u32 *a;
{
	struct prn *p = prn_of(a[0]);

	return p && enddoc(p) >= 0 ? 1 : (u32)-1;
}

static u32
g_StartPage(a)
	u32 *a;
{
	struct prn *p = prn_of(a[0]);

	if (!p || !p->indoc)
		return 0;
	startpage(p);
	return 1;
}

static u32
g_EndPage(a)
	u32 *a;
{
	struct prn *p = prn_of(a[0]);

	if (!p || !p->indoc)
		return (u32)-1;
	if (!p->inpage)
		startpage(p);
	return endpage(p) > 0 ? 1 : (u32)-1;
}

static u32
g_AbortDoc(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);

	return dc && dc->prn ? prn_escape(dc, ABORTDOC, 0, (u32)0, (u32)0) : (u32)-1;
}

static u32
g_SetAbortProc(a)
	u32 *a;
{
	struct prn *p = prn_of(a[0]);

	if (!p)
		return (u32)-1;
	p->abortproc = a[1];
	return 1;
}

/* QueryAbort(hdc, reserved): the program's abort procedure asked */
static u32
g_QueryAbort(a)
	u32 *a;
{
	struct prn *p = prn_of(a[0]);

	return p ? stillgoing(p) : 1;
}

/* ---- the spooler ---- */

#define	NJOB	8

static struct job {
	int	used;
	FILE	*fp;
	char	file[300];	/* on the host */
	char	port[64];
	char	title[80];
} jobs[NJOB];

/* s in single quotes for the shell, onto d (n left): what it took */
static int
shquote(d, n, s)
	char *d, *s;
	int n;
{
	int k = 0;

	if (n > 1)
		d[k++] = '\'';
	for (; *s && k < n - 6; s++)
		if (*s == '\'') {
			strcpy(d + k, "'\\''");
			k += 4;
		} else
			d[k++] = *s;
	if (k < n - 1)
		d[k++] = '\'';
	d[k] = 0;
	return k;
}

/*
 * The command a port's jobs go to: WABI.INI's Printers.command_lptN as
 * Wabi had it, or lp.  In it %p is Printers.name_lptN, the host's
 * printer (a word with %p left out for "<Default Printer>": lp's own),
 * %t the job's title.
 */
static void
portcommand(port, title, cmd, n)
	char *port, *title, *cmd;
	int n;
{
	char key[64], v[300], x[400], name[100], lp[16], *w, *e;
	int i, k, def;

	strcpy(cmd, "lp");
	for (i = 0; port[i] && port[i] != ':' && i < 15; i++)
		lp[i] = port[i] >= 'A' && port[i] <= 'Z' ? port[i] + 32 : port[i];
	lp[i] = 0;
	sprintf(key, "Printers.command_%s", lp);
	profile_get("C:\\WINDOWS\\WABI.INI", "Unknown", key, "", v, (u32)sizeof v);
	if (!v[0])
		return;
	{
		extern void wabi_expand();

		wabi_expand(v, x, (int)sizeof x);
	}
	sprintf(key, "Printers.name_%s", lp);
	profile_get("C:\\WINDOWS\\WABI.INI", "Unknown", key, "", name, (u32)sizeof name);
	def = !name[0] || name[0] == '<';
	k = 0;
	for (w = x; *w && k < n - 1; w = e) {
		for (e = w; *e && *e != ' '; e++)
			;
		while (*e == ' ')
			e++;
		/* a word: left out if it names the default printer */
		for (i = 0; w + i < e && !(w[i] == '%' && w[i + 1] == 'p'); i++)
			;
		if (def && w + i < e)
			continue;
		for (; w < e && k < n - 1; w++) {
			if (w[0] == '%' && (w[1] == 'p' || w[1] == 't')) {
				k += shquote(cmd + k, n - k, w[1] == 'p' ? name : title);
				w++;
			} else
				cmd[k++] = *w;
		}
	}
	while (k > 0 && cmd[k - 1] == ' ')
		k--;
	cmd[k] = 0;
}

/* OpenJob(port, title, hdc): the job's handle, or a negative SP_ error */
static u32
g_OpenJob(a)
	u32 *a;
{
	char *port = gptr(a[0]), *title = gptr(a[1]), dir[300];
	struct job *j;
	int i;
	static int seq;

	for (i = 0; i < NJOB && jobs[i].used; i++)
		;
	if (i == NJOB)
		return (u32)-1;		/* SP_ERROR */
	j = &jobs[i];
	memset((char *)j, 0, sizeof *j);
	if (dos_hostpath("C:\\TEMP", dir, sizeof dir, 1) != 0)
		strcpy(dir, "/tmp");
	sprintf(j->file, "%.250s/~spl%d%d.tmp", dir, (int)getpid() % 1000, ++seq % 100);
	if ((j->fp = fopen(j->file, "wb")) == 0)
		return (u32)-1;
	j->used = 1;
	strncpy(j->port, port ? port : "LPT1:", sizeof j->port - 1);
	strncpy(j->title, title ? title : "", sizeof j->title - 1);
	if (w16_debug)
		w16_log("startwin: print job \"%s\" for %s\n", j->title, j->port);
	return 0x5000 + i;
}

static struct job *
jobof(h)
	u32 h;
{
	int i = (int)(h & 0xffff) - 0x5000;

	return i >= 0 && i < NJOB && jobs[i].used ? &jobs[i] : 0;
}

static u32
g_WriteSpool(a)
	u32 *a;
{
	struct job *j = jobof(a[0]);
	int n = a[2] & 0xffff;
	u32 l = lin(FPSEL(a[1]), FPOFF(a[1]));

	if (!j || !l)
		return (u32)-1;
	if (n && fwrite(M + l, 1, n, j->fp) != (size_t)n)
		return (u32)-4;		/* SP_OUTOFDISK */
	return n;
}

static u32 g_StartSpoolPage(a) u32 *a; { return jobof(a[0]) ? 1 : (u32)-1; }
static u32 g_EndSpoolPage(a) u32 *a; { return jobof(a[0]) ? 1 : (u32)-1; }

/* CloseJob: the job to the port's command */
static u32
g_CloseJob(a)
	u32 *a;
{
	struct job *j = jobof(a[0]);
	char cmd[400], line[800];
	int r;

	if (!j)
		return (u32)-1;
	fclose(j->fp);
	portcommand(j->port, j->title, cmd, sizeof cmd);
	sprintf(line, "%s < '%s'", cmd, j->file);
	r = system(line);
	if (w16_debug)
		w16_log("startwin: print job \"%s\" to `%s': %d\n", j->title, cmd, r);
	unlink(j->file);
	j->used = 0;
	return 1;
}

static u32
g_DeleteJob(a)
	u32 *a;
{
	struct job *j = jobof(a[0]);

	if (!j)
		return (u32)-1;
	fclose(j->fp);
	unlink(j->file);
	j->used = 0;
	return 1;
}

/* ---- the environments of the ports (the drivers' settings) ---- */

#define	NENV	8

static struct env {
	char	port[64];
	u8	*data;
	int	n;
} envs[NENV];

/* SetEnvironment(port, data, size): kept; size 0 drops it */
static u32
g_SetEnvironment(a)
	u32 *a;
{
	char *port = gptr(a[0]);
	u32 l = lin(FPSEL(a[1]), FPOFF(a[1]));
	int n = a[2] & 0xffff, i, f = -1;

	if (!port)
		return 0;
	for (i = 0; i < NENV; i++)
		if (envs[i].data && w16_stricmp(envs[i].port, port) == 0)
			f = i;
	if (f >= 0 && (n == 0 || !l)) {
		free(envs[f].data);
		envs[f].data = 0;
		return (u32)-1;
	}
	if (!l || n <= 0)
		return 0;
	if (f < 0)
		for (i = 0; i < NENV && f < 0; i++)
			if (!envs[i].data)
				f = i;
	if (f < 0)
		return 0;
	free(envs[f].data);
	envs[f].data = (u8 *)malloc(n);
	memcpy(envs[f].data, M + l, n);
	envs[f].n = n;
	strncpy(envs[f].port, port, sizeof envs[f].port - 1);
	return n;
}

/* GetEnvironment(port, buffer, size): its size if no buffer */
static u32
g_GetEnvironment(a)
	u32 *a;
{
	char *port = gptr(a[0]);
	u32 l = lin(FPSEL(a[1]), FPOFF(a[1]));
	int n = a[2] & 0xffff, i;

	if (!port)
		return 0;
	for (i = 0; i < NENV; i++)
		if (envs[i].data && w16_stricmp(envs[i].port, port) == 0) {
			if (!l)
				return envs[i].n;
			if (n > envs[i].n)
				n = envs[i].n;
			memcpy(M + l, envs[i].data, n);
			return n;
		}
	return 0;
}

/* ---- GDI's brute-force functions, on the drivers' monochrome bitmaps ---- */

/* a device bitmap (BITMAP: type, width, height, width bytes, planes, bits a pixel, bits) */
struct dbm {
	int	w, h, wb, bpp;
	u32	bits;		/* far pointer */
	int	seg, scanseg;	/* a huge one's: selectors apart, scan lines a segment */
};

static int
getdbm(fp, d)
	u32 fp;
	struct dbm *d;
{
	u32 l = lin(FPSEL(fp), FPOFF(fp));

	if (!l || GW(l) != 0)
		return 0;
	d->w = GW(l + 2);
	d->h = GW(l + 4);
	d->wb = GW(l + 6);
	d->bpp = M[l + 8] * M[l + 9];
	d->bits = GL(l + 10);
	d->seg = GW(l + 22);
	d->scanseg = GW(l + 24);
	return d->bpp == 1;
}

/* the linear address of a scan line */
static u32
scan(d, y)
	struct dbm *d;
	int y;
{
	u32 sel = FPSEL(d->bits), off = FPOFF(d->bits);

	if (d->seg && d->scanseg) {
		sel += (y / d->scanseg) * d->seg;
		off += (u32)(y % d->scanseg) * d->wb;
	} else
		off += (u32)y * d->wb;
	return lin(sel, off & 0xffff);
}

static int
bit(d, x, y)
	struct dbm *d;
	int x, y;
{
	return M[scan(d, y) + (x >> 3)] >> (7 - (x & 7)) & 1;
}

static void
setbit(d, x, y, v)
	struct dbm *d;
	int x, y, v;
{
	u8 *p = M + scan(d, y) + (x >> 3);

	if (v)
		*p |= 0x80 >> (x & 7);
	else
		*p &= ~(0x80 >> (x & 7));
}

/* our physical brush: style, 8 rows of its pattern (1 white) */
#define	PBRUSHSIZE	12

/* a pixel of a physical brush, 1 white */
static int
brushbit(pb, x, y)
	u32 pb;
	int x, y;
{
	u32 l = pb ? lin(FPSEL(pb), FPOFF(pb)) : 0;

	if (!l)
		return 0;
	return M[l + 2 + (y & 7)] >> (7 - (x & 7)) & 1;
}

/* dmBitBlt(lpDest, x, y, lpSrc, sx, sy, w, h, rop, lpPBrush, lpDrawMode) */
static u32
g_dmBitBlt(a)
	u32 *a;
{
	struct dbm d, s;
	int dx = (short)a[1], dy = (short)a[2], sx = (short)a[4], sy = (short)a[5];
	int w = (short)a[6], h = (short)a[7], x, y, hassrc, idx, rop = (a[8] >> 16) & 0xff;

	if (!getdbm(a[0], &d)) {
		if (w16_debug)
			w16_log("startwin: dmBitBlt to a bitmap not monochrome\n");
		return 0;
	}
	hassrc = a[3] && getdbm(a[3], &s);
	if (w16_debug > 1)
		w16_log("startwin: dmBitBlt to %dx%d (%d, seg %d/%d) at %d,%d from %s %dx%d at %d,%d, %dx%d rop %lx\n", d.w, d.h,
		    d.wb, d.seg, d.scanseg, dx, dy, hassrc ? "bitmap" : "none", hassrc ? s.w : 0, hassrc ? s.h : 0, sx, sy, w, h,
		    (long)a[8]);
	for (y = 0; y < h; y++) {
		if (dy + y < 0 || dy + y >= d.h)
			continue;
		if (hassrc && (sy + y < 0 || sy + y >= s.h))
			continue;
		for (x = 0; x < w; x++) {
			if (dx + x < 0 || dx + x >= d.w)
				continue;
			if (hassrc && (sx + x < 0 || sx + x >= s.w))
				continue;
			idx = (brushbit(a[9], dx + x, dy + y) ? 4 : 0) | (hassrc && bit(&s, sx + x, sy + y) ? 2 : 0) |
			    (bit(&d, dx + x, dy + y) ? 1 : 0);
			setbit(&d, dx + x, dy + y, rop >> idx & 1);
		}
	}
	return 1;
}

/* dmColorInfo(lpDest, rgb, lpPColor): black or white; with no lpPColor the colour of a physical one */
static u32
g_dmColorInfo(a)
	u32 *a;
{
	u32 l = lin(FPSEL(a[2]), FPOFF(a[2])), c = a[1];
	int white;

	if (!l)
		return c ? RGB(255, 255, 255) : 0;
	white = CR_R(c) * 30 + CR_G(c) * 59 + CR_B(c) * 11 >= 12800;
	PL(l, white ? 0xffffffffL : 0);
	return white ? RGB(255, 255, 255) : 0;
}

/* dmRealizeObject(lpDest, style, lpIn, lpOut, lpXform): pens and brushes; with no lpOut the size */
static u32
g_dmRealizeObject(a)
	u32 *a;
{
	int style = (short)a[1], i;
	u32 in = lin(FPSEL(a[2]), FPOFF(a[2])), out = lin(FPSEL(a[3]), FPOFF(a[3]));
	static u8 hatches[6][8] = {
		{ 0xff, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff },	/* HS_HORIZONTAL */
		{ 0xf7, 0xf7, 0xf7, 0xf7, 0xf7, 0xf7, 0xf7, 0xf7 },	/* HS_VERTICAL */
		{ 0x7f, 0xbf, 0xdf, 0xef, 0xf7, 0xfb, 0xfd, 0xfe },	/* HS_FDIAGONAL */
		{ 0xfe, 0xfd, 0xfb, 0xf7, 0xef, 0xdf, 0xbf, 0x7f },	/* HS_BDIAGONAL */
		{ 0xf7, 0xf7, 0xf7, 0xf7, 0x00, 0xf7, 0xf7, 0xf7 },	/* HS_CROSS */
		{ 0x7e, 0xbd, 0xdb, 0xe7, 0xe7, 0xdb, 0xbd, 0x7e }	/* HS_DIAGCROSS */
	};

	if (style < 0)
		return 1;		/* deleted: nothing kept */
	if (style == 1) {		/* OBJ_PEN */
		if (!out)
			return 8;
		if (in) {
			u32 c = GL(in + 6);
			int white = CR_R(c) * 30 + CR_G(c) * 59 + CR_B(c) * 11 >= 12800;

			PW(out, GW(in));
			PW(out + 2, GW(in + 2));
			PL(out + 4, white ? 0xffffffffL : 0);
		}
		return 8;
	}
	if (style == 2) {		/* OBJ_BRUSH */
		if (!out)
			return PBRUSHSIZE;
		if (in) {
			int bs = GW(in), white;
			u32 c = GL(in + 2);

			white = CR_R(c) * 30 + CR_G(c) * 59 + CR_B(c) * 11 >= 12800;
			PW(out, bs);
			for (i = 0; i < 8; i++)
				M[out + 2 + i] = bs == 1 ? 0xff : bs == 2 && GW(in + 6) < 6 ?
				    (white ? 0xff : hatches[GW(in + 6)][i]) : white ? 0xff : 0x00;
			/* a pattern: its bitmap's 8x8 */
			if (bs == 3) {
				struct dbm pd;

				if (getdbm(GL(in + 6), &pd))
					for (i = 0; i < 8; i++)
						M[out + 2 + i] = i < pd.h ? M[scan(&pd, i)] : 0xff;
			}
		}
		return PBRUSHSIZE;
	}
	return 0;
}

/* dmPixel(lpDest, x, y, color, lpDrawMode): with no draw mode, the pixel's colour */
static u32
g_dmPixel(a)
	u32 *a;
{
	struct dbm d;
	int x = (short)a[1], y = (short)a[2];

	if (!getdbm(a[0], &d) || x < 0 || y < 0 || x >= d.w || y >= d.h)
		return 0x80000000L;
	if (!a[4])
		return bit(&d, x, y) ? 0xffffffffL : 0;
	setbit(&d, x, y, a[3] != 0);
	return 1;
}

/* dmOutput(lpDest, style, count, points, pen, brush, drawmode, clip): scan lines and rectangles */
static u32
g_dmOutput(a)
	u32 *a;
{
	struct dbm d;
	int style = a[1] & 0xffff, n = a[2] & 0xffff, i, x, y;
	u32 pts = lin(FPSEL(a[3]), FPOFF(a[3]));

	if (!getdbm(a[0], &d) || !pts)
		return 0;
	if (style == 4) {		/* OS_SCANLINES: y, then x pairs, filled with the brush */
		y = (short)GW(pts + 2);
		for (i = 1; i < n; i++) {
			int x0 = (short)GW(pts + 4 * i), x1 = (short)GW(pts + 4 * i + 2);

			if (y < 0 || y >= d.h)
				continue;
			for (x = x0 < 0 ? 0 : x0; x < x1 && x < d.w; x++)
				setbit(&d, x, y, brushbit(a[5], x, y));
		}
		return 1;
	}
	if (style == 6 && n >= 2) {	/* OS_RECTANGLE */
		int l = (short)GW(pts), t = (short)GW(pts + 2), r = (short)GW(pts + 4), b = (short)GW(pts + 6);

		for (y = t < 0 ? 0 : t; y < b && y < d.h; y++)
			for (x = l < 0 ? 0 : l; x < r && x < d.w; x++)
				setbit(&d, x, y, (a[5] ? brushbit(a[5], x, y) : 1) &&
				    !(a[4] && (y == t || y == b - 1 || x == l || x == r - 1)));
		return 1;
	}
	/*
	 * 0x7f-0x81: Wabi's own, from its drivers (WBIEPSON) about a
	 * surface's bits (the pointer, the word at +0x40 the count) before
	 * and after they touch them; Wabi kept bitmaps in the X server.
	 * Ours are always in memory: nothing to do.
	 */
	if (style >= 0x7f && style <= 0x81)
		return 1;
	if (w16_debug)
		w16_log("startwin: dmOutput style %d not drawn\n", style);
	return 0;
}

/* dmScanLR(lpDest, x, y, color, style): the first pixel of the colour (or not, style 1) right (or left, 2) */
static u32
g_dmScanLR(a)
	u32 *a;
{
	struct dbm d;
	int x = (short)a[1], y = (short)a[2], st = a[4] & 0xffff, want = a[3] != 0, step = st & 2 ? -1 : 1;

	if (!getdbm(a[0], &d) || y < 0 || y >= d.h)
		return (u32)-1;
	for (; x >= 0 && x < d.w; x += step)
		if ((bit(&d, x, y) == want) != ((st & 1) != 0))
			return x;
	return (u32)-1;
}

/* dmEnumObj(lpDest, style, callback, data): black and white pens or brushes */
static u32
g_dmEnumObj(a)
	u32 *a;
{
	u32 o = ualloc(8), l = ulin(o), r = 1;
	int i;

	for (i = 0; i < 2 && r; i++) {
		memset(M + l, 0, 8);
		if ((a[1] & 0xffff) == 1) {
			PL(l + 6, i ? RGB(255, 255, 255) : 0);
		} else
			PL(l + 2, i ? RGB(255, 255, 255) : 0);
		cb_begin();
		cb_push32(o);
		cb_push32(a[3]);
		r = cb_call(a[2], 0) & 0xffff;
	}
	ufree(o);
	return r;
}

/* dmTranspose(src, dst, count): 8 rows of bytes count wide into count columns of 8 */
static u32
g_dmTranspose(a)
	u32 *a;
{
	u32 s = lin(FPSEL(a[0]), FPOFF(a[0])), d = lin(FPSEL(a[1]), FPOFF(a[1]));
	int n = a[2] & 0xffff, i, j, k;
	u8 out;

	if (!s || !d)
		return 0;
	for (i = 0; i < n; i++)
		for (k = 0; k < 8; k++) {
			out = 0;
			for (j = 0; j < 8; j++)
				if (M[s + j * n + i] & (0x80 >> k))
					out |= 0x80 >> j;
			M[d + i * 8 + k] = out;
		}
	return 1;
}

/* dmStrBlt, dmExtTextOut: text is drawn into the bands by us, never given to the driver */
static u32
g_dmText(a)
	u32 *a;
{
	(void)a;
	return 0;
}

/* ---- priority queues (CreatePQ ...), for the drivers that keep their fonts in one ---- */

#define	NPQ	8

static struct pq {
	int	used, n, max;
	u32	*tag, *key;
} pqs[NPQ];

static struct pq *
pqof(h)
	u32 h;
{
	int i = (int)(h & 0xffff) - 0x6000;

	return i >= 0 && i < NPQ && pqs[i].used ? &pqs[i] : 0;
}

static u32
g_CreatePQ(a)
	u32 *a;
{
	int i, n = a[0] & 0xffff;

	for (i = 0; i < NPQ && pqs[i].used; i++)
		;
	if (i == NPQ || n <= 0)
		return 0;
	pqs[i].used = 1;
	pqs[i].n = 0;
	pqs[i].max = n;
	pqs[i].tag = (u32 *)calloc(n, sizeof (u32));
	pqs[i].key = (u32 *)calloc(n, sizeof (u32));
	return 0x6000 + i;
}

/* InsertPQ(hPQ, tag, key) */
static u32
g_InsertPQ(a)
	u32 *a;
{
	struct pq *q = pqof(a[0]);

	if (!q || q->n >= q->max)
		return (u32)-1;
	q->tag[q->n] = a[1] & 0xffff;
	q->key[q->n] = a[2] & 0xffff;
	return ++q->n;
}

static int
pqmin(q)
	struct pq *q;
{
	int i, m = 0;

	for (i = 1; i < q->n; i++)
		if (q->key[i] < q->key[m])
			m = i;
	return m;
}

static u32
g_MinPQ(a)
	u32 *a;
{
	struct pq *q = pqof(a[0]);

	return q && q->n ? q->tag[pqmin(q)] : (u32)-1;
}

static u32
g_ExtractPQ(a)
	u32 *a;
{
	struct pq *q = pqof(a[0]);
	int m;
	u32 t;

	if (!q || !q->n)
		return (u32)-1;
	m = pqmin(q);
	t = q->tag[m];
	q->n--;
	q->tag[m] = q->tag[q->n];
	q->key[m] = q->key[q->n];
	return t;
}

/* SizePQ(hPQ, more): room for that many more */
static u32
g_SizePQ(a)
	u32 *a;
{
	struct pq *q = pqof(a[0]);
	int more = (short)a[1];

	if (!q)
		return (u32)-1;
	if (q->max + more < q->n)
		return (u32)-1;
	q->max += more;
	q->tag = (u32 *)realloc((char *)q->tag, (q->max + 1) * sizeof (u32));
	q->key = (u32 *)realloc((char *)q->key, (q->max + 1) * sizeof (u32));
	return q->max;
}

static u32
g_DeletePQ(a)
	u32 *a;
{
	struct pq *q = pqof(a[0]);

	if (!q)
		return (u32)-1;
	free(q->tag);
	free(q->key);
	q->used = 0;
	return 1;
}

struct impl pr_impl[] = {
	{ "GDI", "StartDoc", g_StartDoc },
	{ "GDI", "EndDoc", g_EndDoc },
	{ "GDI", "StartPage", g_StartPage },
	{ "GDI", "EndPage", g_EndPage },
	{ "GDI", "AbortDoc", g_AbortDoc },
	{ "GDI", "SetAbortProc", g_SetAbortProc },
	{ "GDI", "QueryAbort", g_QueryAbort },
	{ "GDI", "OpenJob", g_OpenJob },
	{ "GDI", "WriteSpool", g_WriteSpool },
	{ "GDI", "StartSpoolPage", g_StartSpoolPage },
	{ "GDI", "EndSpoolPage", g_EndSpoolPage },
	{ "GDI", "CloseJob", g_CloseJob },
	{ "GDI", "DeleteJob", g_DeleteJob },
	{ "GDI", "SetEnvironment", g_SetEnvironment },
	{ "GDI", "GetEnvironment", g_GetEnvironment },
	{ "GDI", "DMBITBLT", g_dmBitBlt },
	{ "GDI", "DMCOLORINFO", g_dmColorInfo },
	{ "GDI", "DMENUMOBJ", g_dmEnumObj },
	{ "GDI", "DMOUTPUT", g_dmOutput },
	{ "GDI", "DMPIXEL", g_dmPixel },
	{ "GDI", "dmRealizeObject", g_dmRealizeObject },
	{ "GDI", "DMSTRBLT", g_dmText },
	{ "GDI", "DMSCANLR", g_dmScanLR },
	{ "GDI", "DMEXTTEXTOUT", g_dmText },
	{ "GDI", "DMTRANSPOSE", g_dmTranspose },
	{ "GDI", "CreatePQ", g_CreatePQ },
	{ "GDI", "MinPQ", g_MinPQ },
	{ "GDI", "ExtractPQ", g_ExtractPQ },
	{ "GDI", "InsertPQ", g_InsertPQ },
	{ "GDI", "SizePQ", g_SizePQ },
	{ "GDI", "DeletePQ", g_DeletePQ },
	{ 0 }
};
