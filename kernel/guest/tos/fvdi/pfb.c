/*
 * pfb.c -- the frame buffer driver on separate bitplanes.  Bitmaps in
 * the device format are the VDI standard format: whole planes, one
 * after the other.
 */
#include "fvdi.h"
#include "driver.h"
#include "tosfb.h"
#include "fbops.h"
#include "plane.h"

static struct plbm scr;
static struct plsave msave;
static unsigned short mdata[32];
static long mfg, mbg;

static long where(Workstation *wk, MFDB *m, struct plbm *b)
{
	if (!m || !m->address || m->address == wk->screen.mfdb.address) {
		*b = scr;
		return 1;
	}
	if (m->bitplanes != scr.planes)
		return 0;
	b->base = (unsigned char *)m->address;
	b->stride = (long)m->wdwidth * 2;
	b->pstep = b->stride * m->height;
	b->planes = scr.planes;
	return 1;
}

static long CDECL p_write_pixel(Virtual *vwk, MFDB *dst, long x, long y, long colour)
{
	struct plbm b;

	if ((long)vwk & 1)
		return 0;
	if (!where(vwk->real_address, dst, &b))
		return 0;
	pl_put(&b, x, y, colour);
	return 1;
}

static long CDECL p_read_pixel(Virtual *vwk, MFDB *src, long x, long y)
{
	struct plbm b;

	if (!where(vwk->real_address, src, &b))
		return 0;
	return pl_get(&b, x, y);
}

static long CDECL p_fill_area(Virtual *vwk, long x, long y, long w, long h,
    short *pattern, long colour, long mode, long interior_style)
{
	unsigned long fg, bg;

	(void)interior_style;
	if ((long)vwk & 1)
		return -1;
	if (w > 0 && h > 0) {
		c_get_colours(vwk, colour, &fg, &bg);
		pl_fill(&scr, x, y, w, h, (unsigned short *)pattern, fg, bg, mode);
	}
	return 1;
}

static long CDECL p_expand_area(Virtual *vwk, MFDB *src, long src_x, long src_y,
    MFDB *dst, long dst_x, long dst_y, long w, long h, long operation, long colour)
{
	unsigned long fg, bg;
	struct plbm b;
	long swrap;

	if (w <= 0 || h <= 0)
		return 1;
	if (!where(vwk->real_address, dst, &b))
		return 0;
	c_get_colours(vwk, colour, &fg, &bg);
	swrap = (long)src->wdwidth * 2;
	pl_expand(&b, dst_x, dst_y, w, h, (char *)src->address + src_y * swrap, swrap, src_x,
	    fg, bg, operation, 0);
	return 1;
}

static long CDECL p_blit_area(Virtual *vwk, MFDB *src, long src_x, long src_y,
    MFDB *dst, long dst_x, long dst_y, long w, long h, long operation)
{
	Workstation *wk = vwk->real_address;
	struct plbm s, d;

	if (w <= 0 || h <= 0)
		return 1;
	if (!where(wk, src, &s) || !where(wk, dst, &d))
		return 0;
	pl_blit(&s, src_x, src_y, &d, dst_x, dst_y, w, h, operation);
	return 1;
}

static long CDECL p_line_draw(Virtual *vwk, long x1, long y1, long x2, long y2,
    long pattern, long colour, long mode)
{
	unsigned long fg, bg;

	if ((long)vwk & 1)
		return -1;
	if (clip_line(vwk, &x1, &y1, &x2, &y2)) {
		c_get_colours(vwk, colour, &fg, &bg);
		pl_line(&scr, x1, y1, x2, y2, pattern, fg, bg, mode);
	}
	return 1;
}

static void mouse_show(Workstation *wk, long x, long y)
{
	unsigned short *m = mdata;
	long w = 16, h = 16, sh = 0;

	x -= wk->mouse.hotspot.x;
	y -= wk->mouse.hotspot.y;
	if (y < 0) {
		h += y;
		m -= 2 * y;
		y = 0;
	}
	if (y + h > wk->screen.mfdb.height)
		h = wk->screen.mfdb.height - y;
	if (x < 0) {
		w += x;
		sh = -x;
		x = 0;
	}
	if (x + w > wk->screen.mfdb.width)
		w = wk->screen.mfdb.width - x;
	if (w > 0 && h > 0)
		pl_mshow(&scr, &msave, x, y, w, h, sh, m, mfg, mbg);
}

/* 0 move, 2 hide, 3 show, 4 move; above 7 a new shape */
static long CDECL p_mouse_draw(Workstation *wk, long x, long y, Mouse *mouse)
{
	long parm = (long)mouse;
	short i;

	if (parm > 7) {
		mfg = fb_pen(wk->mouse.colour.foreground);
		mbg = fb_pen(wk->mouse.colour.background);
		for (i = 0; i < 16; i++) {
			mdata[2 * i] = mouse->mask[i];
			mdata[2 * i + 1] = mouse->data[i];
		}
		return 0;
	}
	if (parm == 0 || parm == 2 || parm == 3 || parm == 4)
		pl_mhide(&scr, &msave);
	if (parm == 0 || parm == 3 || parm == 4)
		mouse_show(wk, x & 0xffff, y);
	return 0;
}

/* the planes at fc_addr, fc_plane8 * 8 bytes apart */
void pfb_init(Workstation *wk, struct tfbcart *fc)
{
	scr.base = (unsigned char *)fc->fc_addr;
	scr.stride = fc->fc_rowbytes;
	scr.pstep = (long)fc->fc_plane8 * 8;
	scr.planes = fc->fc_depth;
	wk->screen.mfdb.wdwidth = (fc->fc_width + 15) / 16;
	write_pixel_r = p_write_pixel;
	read_pixel_r = p_read_pixel;
	line_draw_r = p_line_draw;
	expand_area_r = p_expand_area;
	fill_area_r = p_fill_area;
	blit_area_r = p_blit_area;
	mouse_draw_r = p_mouse_draw;
}
