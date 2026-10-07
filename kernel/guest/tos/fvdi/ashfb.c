/*
 * ashfb.c -- fVDI driver for the container's frame buffer: 8-bit
 * chunky pixels at the address the launcher put in the cartridge.
 * The palette goes to the display process through ST-RAM (tosfb.h).
 */
#include "fvdi.h"
#include "driver.h"
#include "tosfb.h"
#include "string/memset.h"
#include "fbops.h"
#include "../../drawops/drawops.h"

static char const red[] = { 8 };
static char const green[] = { 8 };
static char const blue[] = { 8 };
static char const none[] = { 0 };

static Mode const mode[1] = {
	{ 8, CHUNKY, { red, green, blue, none, none, none }, 0, 2, 1, 1 }
};

char driver_name[] = "Ash Nazag frame buffer";

static struct tfbshare *share;
static Pix pens[256];

static long pen(long c)
{
	return pens[c & 0xff];
}

long fb_pen(long c)
{
	return pen(c);
}

long CDECL c_get_colour(Virtual *vwk, long colour)
{
	(void)vwk;
	return (pen(colour >> 16) << 16) | pen(colour);
}

void CDECL c_get_colours(Virtual *vwk, long colour, unsigned long *fg, unsigned long *bg)
{
	(void)vwk;
	*fg = pen(colour);
	*bg = pen(colour >> 16);
}

/* VDI 0..1000 or, with bit 0 of requested set, 16-bit components */
void CDECL c_set_colours(Virtual *vwk, long start, long entries, unsigned short *requested, Colour palette[])
{
	long i, k, v[3], p;
	int fine = (long)requested & 1;
	Colour *c;

	(void)vwk;
	requested = (unsigned short *)((long)requested & ~1L);
	for (i = 0; i < entries; i++) {
		if (fine)
			requested++;
		c = &palette[start + i];
		p = pen(start + i);
		for (k = 0; k < 3; k++) {
			if (fine)
				v[k] = *requested++;
			else {
				v[k] = (short)*requested++;
				v[k] = v[k] < 0 ? 0 : v[k] > 1000 ? 65535 : v[k] * 65535L / 1000;
			}
			if (share)
				share->fs_pal[p][k] = v[k];
		}
		c->vdi.red = v[0] * 1000L / 65535;
		c->vdi.green = v[1] * 1000L / 65535;
		c->vdi.blue = v[2] * 1000L / 65535;
		c->hw.red = v[0] >> 8;
		c->hw.green = v[1] >> 8;
		c->hw.blue = v[2] >> 8;
		c->real = p;
	}
	if (share)
		share->fs_seq++;
}

/* a destination or source: the screen, or an 8-bit bitmap; 0 if neither */
static Pix *where(Workstation *wk, MFDB *m, long *wrap)
{
	if (!m || !m->address || m->address == wk->screen.mfdb.address || la_alias(m->address)) {
		*wrap = wk->screen.wrap;
		return (Pix *)wk->screen.mfdb.address;
	}
	if (m->bitplanes != 8)
		return 0;
	*wrap = (long)m->wdwidth * 2 * 8;
	return (Pix *)m->address;
}

long CDECL c_write_pixel(Virtual *vwk, MFDB *dst, long x, long y, long colour)
{
	Pix *a;
	long wrap;

	if ((long)vwk & 1)
		return 0;
	if (!(a = where(vwk->real_address, dst, &wrap)))
		return 0;
	a[y * wrap + x] = colour;
	return 1;
}

long CDECL c_read_pixel(Virtual *vwk, MFDB *src, long x, long y)
{
	Pix *a;
	long wrap;

	if (!(a = where(vwk->real_address, src, &wrap)))
		return 0;
	return a[y * wrap + x];
}

/* the drawing library on the driver's screen or an 8-bit bitmap */
static void dop(struct do_op *o, long kind, Pix *d, long dwrap, long x, long y, long w, long h)
{
	o->kind = kind;
	o->depth = 8;
	o->dst = d;
	o->dstride = dwrap;
	o->x = x;
	o->y = y;
	o->w = w;
	o->h = h;
	o->pmask = 0xff;
	o->src = 0;
	o->sstride = o->sx = o->sy = 0;
	o->pat = 0;
	o->style = 0;
	o->flags = 0;
}

static void sw_fill(Workstation *wk, long x, long y, long w, long h,
    unsigned short const *pattern, long fg, long bg, long mode)
{
	struct do_op o;

	dop(&o, DO_FILL, (Pix *)wk->screen.mfdb.address, wk->screen.wrap, x, y, w, h);
	o.pat = pattern;
	o.fg = fg;
	o.bg = bg;
	o.mode = mode;
	do_draw(&o);
}

long CDECL c_fill_area(Virtual *vwk, long x, long y, long w, long h,
    short *pattern, long colour, long mode, long interior_style)
{
	unsigned long fg, bg;

	(void)interior_style;
	if ((long)vwk & 1)
		return -1;
	if (w > 0 && h > 0) {
		c_get_colours(vwk, colour, &fg, &bg);
		fb->fill(vwk->real_address, x, y, w, h, (unsigned short *)pattern, fg, bg, mode);
	}
	return 1;
}

static void sw_expand(unsigned short const *src, long swrap, long sx,
    Pix *drow, long dwrap, long w, long h, long fg, long bg, long mode)
{
	struct do_op o;

	dop(&o, DO_EXPAND, drow, dwrap, 0, 0, w, h);
	o.src = src;
	o.sstride = swrap;
	o.sx = sx;
	o.fg = fg;
	o.bg = bg;
	o.mode = mode;
	do_draw(&o);
}

long CDECL c_expand_area(Virtual *vwk, MFDB *src, long src_x, long src_y,
    MFDB *dst, long dst_x, long dst_y, long w, long h, long operation, long colour)
{
	unsigned long fg, bg;
	long swrap, dwrap;
	Pix *d;

	if (w <= 0 || h <= 0)
		return 1;
	if (!(d = where(vwk->real_address, dst, &dwrap)))
		return 0;
	c_get_colours(vwk, colour, &fg, &bg);
	swrap = (long)src->wdwidth * 2;
	fb->expand((unsigned short *)((char *)src->address + src_y * swrap), swrap, src_x,
	    d + dst_y * dwrap + dst_x, dwrap, w, h, fg, bg, operation);
	return 1;
}

/* the 16 logic operations on a source and destination pixel */
Pix fb_op(long o, Pix s, Pix d)
{
	switch (o) {
	case 0: return 0;
	case 1: return s & d;
	case 2: return s & ~d;
	case 3: return s;
	case 4: return ~s & d;
	case 5: return d;
	case 6: return s ^ d;
	case 7: return s | d;
	case 8: return ~(s | d);
	case 9: return ~(s ^ d);
	case 10: return ~d;
	case 11: return s | ~d;
	case 12: return ~s;
	case 13: return ~s | d;
	case 14: return ~(s & d);
	default: return 0xff;
	}
}

static void sw_blit(Pix *s, long swrap, Pix *d, long dwrap, long w, long h, long operation)
{
	struct do_op o;

	dop(&o, DO_BLIT, d, dwrap, 0, 0, w, h);
	o.src = s;
	o.sstride = swrap;
	o.mode = operation;
	do_draw(&o);
}

long CDECL c_blit_area(Virtual *vwk, MFDB *src, long src_x, long src_y,
    MFDB *dst, long dst_x, long dst_y, long w, long h, long operation)
{
	Workstation *wk = vwk->real_address;
	Pix *s, *d;
	long swrap, dwrap;

	if (w <= 0 || h <= 0)
		return 1;
	if (!(s = where(wk, src, &swrap)) || !(d = where(wk, dst, &dwrap)))
		return 0;
	fb->blit(s + src_y * swrap + src_x, swrap, d + dst_y * dwrap + dst_x, dwrap, w, h, operation);
	return 1;
}

static void sw_line(Workstation *wk, long x1, long y1, long x2, long y2,
    long pattern, long fg, long bg, long mode)
{
	struct do_op o;

	dop(&o, DO_LINE, (Pix *)wk->screen.mfdb.address, wk->screen.wrap, x1, y1, x2, y2);
	o.style = pattern & 0xffff;
	o.fg = fg;
	o.bg = bg;
	o.mode = mode;
	do_draw(&o);
}

long CDECL c_line_draw(Virtual *vwk, long x1, long y1, long x2, long y2,
    long pattern, long colour, long mode)
{
	unsigned long fg, bg;

	if ((long)vwk & 1)
		return -1;
	if (clip_line(vwk, &x1, &y1, &x2, &y2)) {
		c_get_colours(vwk, colour, &fg, &bg);
		fb->line(vwk->real_address, x1, y1, x2, y2, pattern, fg, bg, mode);
	}
	return 1;
}

static struct fbops const soft = { sw_fill, sw_expand, sw_blit, sw_line };

/* the host's own drawing when it has one; the CPU otherwise */
struct fbops const *fb = &soft;
Workstation *fb_wk;

/* the pointer: 16x16, what it covers saved while it shows */
static unsigned short mdata[32] = {
	0xffff, 0x0000, 0x7ffe, 0x3ffc, 0x3ffc, 0x1ff8, 0x1ff8, 0x0ff0,
	0x0ff0, 0x07e0, 0x07e0, 0x03c0, 0x03c0, 0x0180, 0x0180, 0x0000
};
static Pix msave[256], mfg = 0xff, mbg = 0;
static Pix *mat;
static short mw, mh;

static void mouse_hide(Workstation *wk)
{
	Pix *d = mat, *s = msave;
	short i, j;

	for (j = 0; j < mh; j++, d += wk->screen.wrap)
		for (i = 0; i < mw; i++)
			d[i] = *s++;
	mat = 0;
}

static void mouse_show(Workstation *wk, long x, long y)
{
	unsigned short *m = mdata, bgm, fgm;
	short sh = 0, i, j;
	Pix *d, *s = msave;

	x -= wk->mouse.hotspot.x;
	y -= wk->mouse.hotspot.y;
	mw = mh = 16;
	if (y < 0) {
		mh += y;
		m -= 2 * y;
		y = 0;
	}
	if (y + mh > wk->screen.mfdb.height)
		mh = wk->screen.mfdb.height - y;
	if (x < 0) {
		mw += x;
		sh = -x;
		x = 0;
	}
	if (x + mw > wk->screen.mfdb.width)
		mw = wk->screen.mfdb.width - x;
	if (mw <= 0 || mh <= 0)
		return;
	mat = (Pix *)wk->screen.mfdb.address + y * wk->screen.wrap + x;
	for (d = mat, j = 0; j < mh; j++, d += wk->screen.wrap) {
		bgm = *m++ << sh;
		fgm = *m++ << sh;
		for (i = 0; i < mw; i++, bgm <<= 1, fgm <<= 1) {
			*s++ = d[i];
			if (fgm & 0x8000)
				d[i] = mfg;
			else if (bgm & 0x8000)
				d[i] = mbg;
		}
	}
}

/* 0 move, 2 hide, 3 show, 4 move; above 7 a new shape */
long CDECL c_mouse_draw(Workstation *wk, long x, long y, Mouse *mouse)
{
	long parm = (long)mouse;
	short i;

	if (parm > 7) {
		mfg = pen(wk->mouse.colour.foreground);
		mbg = pen(wk->mouse.colour.background);
		for (i = 0; i < 16; i++) {
			mdata[2 * i] = mouse->mask[i];
			mdata[2 * i + 1] = mouse->data[i];
		}
		return 0;
	}
	if (mat && (parm == 0 || parm == 2 || parm == 3 || parm == 4))
		mouse_hide(wk);
	if (parm == 0 || parm == 3 || parm == 4)
		mouse_show(wk, x & 0xffff, y);
	return 0;
}

long CDECL (*write_pixel_r)(Virtual *, MFDB *, long, long, long) = c_write_pixel;
long CDECL (*read_pixel_r)(Virtual *, MFDB *, long, long) = c_read_pixel;
long CDECL (*line_draw_r)(Virtual *, long, long, long, long, long, long, long) = c_line_draw;
long CDECL (*expand_area_r)(Virtual *, MFDB *, long, long, MFDB *, long, long, long, long, long, long) = c_expand_area;
long CDECL (*fill_area_r)(Virtual *, long, long, long, long, short *, long, long, long) = c_fill_area;
long CDECL (*fill_poly_r)(Virtual *, short[], long, short[], long, short *, long, long, long) = 0;
long CDECL (*blit_area_r)(Virtual *, MFDB *, long, long, MFDB *, long, long, long, long, long) = c_blit_area;
long CDECL (*text_area_r)(Virtual *, short *, long, long, long, short *) = 0;
long CDECL (*mouse_draw_r)(Workstation *, long, long, Mouse *) = c_mouse_draw;
long CDECL (*get_colour_r)(Virtual *, long) = c_get_colour;
void CDECL (*get_colours_r)(Virtual *, long, unsigned long *, unsigned long *) = 0;
void CDECL (*set_colours_r)(Virtual *, long, long, unsigned short *, Colour[]) = c_set_colours;

long wk_extend = 0;
short accel_s = 0;
short accel_c = A_SET_PAL | A_GET_COL | A_SET_PIX | A_GET_PIX | A_BLIT | A_FILL | A_EXPAND | A_LINE | A_MOUSE;
const Mode *graphics_mode = &mode[0];

long check_token(char *token, const char **ptr)
{
	(void)token;
	(void)ptr;
	return 0;
}

long CDECL initialize(Virtual *vwk)
{
	struct tfbcart *fc = (struct tfbcart *)TFB_CART;
	static signed char tos[16] = { 0, -1, 1, 2, 4, 6, 3, 5, 7, 8, 9, 10, 12, 14, 11, 13 };
	Workstation *wk;
	long i;

	if (*(long *)0xfa0000 != 0xabcdef42L || !fc->fc_addr || fc->fc_depth != 8) {
		access->funcs.error("ashfb: no frame buffer", 0);
		return 0;
	}
	for (i = 0; i < 256; i++)
		pens[i] = i < 16 ? (tos[i] < 0 ? 255 : tos[i]) : i == 255 ? 15 : i;
	vwk = me->default_vwk;
	wk = vwk->real_address;
	wk->screen.mfdb.address = (void *)fc->fc_addr;
	wk->screen.mfdb.width = fc->fc_width;
	wk->screen.mfdb.height = fc->fc_height;
	wk->screen.mfdb.bitplanes = 8;
	wk->screen.mfdb.wdwidth = (fc->fc_width + 15) / 16;
	wk->screen.mfdb.standard = 0;
	wk->screen.wrap = fc->fc_rowbytes;
	wk->screen.look_up_table = 0;
	wk->screen.coordinates.max_x = fc->fc_width - 1;
	wk->screen.coordinates.max_y = fc->fc_height - 1;
	wk->screen.pixel.width = 25400 / 72;
	wk->screen.pixel.height = 25400 / 72;
	wk->screen.palette.size = 256;
	wk->screen.palette.colours = (Colour *)access->funcs.malloc(256 * sizeof(Colour), 3);
	share = (struct tfbshare *)access->funcs.malloc(sizeof *share, 0);
	if (!wk->screen.palette.colours || !share) {
		access->funcs.error("ashfb: out of memory", 0);
		return 0;
	}
	share->fs_magic = TFB_MAGIC;
	share->fs_seq = 0;
	if (loaded_palette)
		access->funcs.copymem(loaded_palette, default_vdi_colors, 256 * 3 * sizeof(short));
	c_set_colours(vwk, 0, 256, (unsigned short *)default_vdi_colors, wk->screen.palette.colours);
	/* packed pixels: palette calls stay here instead of going to the ROM's VDI */
	device.format = 2;
	device.clut = 1;
	device.bit_depth = 8;
	device.colours = 256;
	device.byte_width = wk->screen.wrap;
	device.address = wk->screen.mfdb.address;
	wk->mouse.position.x = fc->fc_width / 2;
	wk->mouse.position.y = fc->fc_height / 2;
	share->fs_on = 1;
	access->funcs.set_cookie("AshF", (long)share);
	fb_wk = wk;
	do_init(access->funcs.get_cookie("_CPU", 0));
	linea_init(wk);
	return 1;
}

long CDECL setup(long type, long value)
{
	if (type == Q_NAME)
		return (long)driver_name;
	if (type == S_DRVOPTION)
		return tokenize((char *)value);
	return -1;
}

Virtual *CDECL opnwk(Virtual *vwk)
{
	(void)vwk;
	return 0;
}

void CDECL clswk(Virtual *vwk)
{
	(void)vwk;
}
