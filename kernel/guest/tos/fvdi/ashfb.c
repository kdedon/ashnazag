/*
 * ashfb.c -- fVDI driver for the container's frame buffer: 8-bit
 * chunky pixels at the address the launcher put in the cartridge.
 * The palette goes to the display process through ST-RAM (tosfb.h).
 */
#include "fvdi.h"
#include "driver.h"
#include "tosfb.h"
#include "string/memset.h"

typedef unsigned char Pix;

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
	if (!m || !m->address || m->address == wk->screen.mfdb.address) {
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

/* one pixel by mode: 1 replace, 2 transparent, 3 xor, 4 reverse transparent */
#define	PUT(d, bit, mode, fg, bg) \
	switch (mode) { \
	case 1: *(d) = (bit) ? (fg) : (bg); break; \
	case 2: if (bit) *(d) = (fg); break; \
	case 3: if (bit) *(d) = ~*(d); break; \
	default: if (!(bit)) *(d) = (fg); break; \
	}

long CDECL c_fill_area(Virtual *vwk, long x, long y, long w, long h,
    short *pattern, long colour, long mode, long interior_style)
{
	Workstation *wk;
	unsigned long fg, bg;
	unsigned short pw;
	Pix *row, *d;
	long i, j;

	(void)interior_style;
	if ((long)vwk & 1)
		return -1;
	if (w <= 0 || h <= 0)
		return 1;
	wk = vwk->real_address;
	c_get_colours(vwk, colour, &fg, &bg);
	row = (Pix *)wk->screen.mfdb.address + y * wk->screen.wrap + x;
	for (j = 0; j < h; j++, row += wk->screen.wrap) {
		pw = pattern[(y + j) & 15];
		if (mode == 1 && (pw == 0xffff || pw == 0)) {
			Pix c = pw ? fg : bg;
			for (d = row, i = w; i > 0; i--)
				*d++ = c;
			continue;
		}
		pw = (pw << (x & 15)) | (pw >> (16 - (x & 15)));
		for (d = row, i = 0; i < w; i++, d++) {
			PUT(d, pw & 0x8000, mode, fg, bg);
			pw = (pw << 1) | (pw >> 15);
		}
	}
	return 1;
}

long CDECL c_expand_area(Virtual *vwk, MFDB *src, long src_x, long src_y,
    MFDB *dst, long dst_x, long dst_y, long w, long h, long operation, long colour)
{
	Workstation *wk = vwk->real_address;
	unsigned long fg, bg;
	unsigned short *s, word, mask;
	long swrap, dwrap, i, j;
	Pix *drow, *d;

	if (w <= 0 || h <= 0)
		return 1;
	if (!(drow = where(wk, dst, &dwrap)))
		return 0;
	c_get_colours(vwk, colour, &fg, &bg);
	swrap = (long)src->wdwidth * 2;
	drow += dst_y * dwrap + dst_x;
	for (j = 0; j < h; j++, drow += dwrap) {
		s = (unsigned short *)((char *)src->address + (src_y + j) * swrap) + (src_x >> 4);
		word = *s++;
		mask = 0x8000 >> (src_x & 15);
		for (d = drow, i = 0; i < w; i++, d++) {
			PUT(d, word & mask, operation, fg, bg);
			if (!(mask >>= 1)) {
				mask = 0x8000;
				word = *s++;
			}
		}
	}
	return 1;
}

/* the 16 logic operations on a source and destination pixel */
static Pix op(long o, Pix s, Pix d)
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

long CDECL c_blit_area(Virtual *vwk, MFDB *src, long src_x, long src_y,
    MFDB *dst, long dst_x, long dst_y, long w, long h, long operation)
{
	Workstation *wk = vwk->real_address;
	Pix *s, *d;
	long swrap, dwrap, i, j, step = 1;

	if (w <= 0 || h <= 0)
		return 1;
	if (!(s = where(wk, src, &swrap)) || !(d = where(wk, dst, &dwrap)))
		return 0;
	s += src_y * swrap + src_x;
	d += dst_y * dwrap + dst_x;
	if (s == d && operation == 3)
		return 1;
	if (s < d && s + h * swrap > d) {	/* overlapping, source above: bottom up */
		s += (h - 1) * swrap;
		d += (h - 1) * dwrap;
		swrap = -swrap;
		dwrap = -dwrap;
	}
	if (s < d && s + w > d && swrap == dwrap) {	/* same rows, source to the left */
		step = -1;
		s += w - 1;
		d += w - 1;
	}
	for (j = 0; j < h; j++, s += swrap, d += dwrap) {
		if (operation == 3 && step == 1) {
			Pix *a = s, *b = d;
			for (i = w; i >= 4 && !(((long)a | (long)b) & 1); i -= 4, a += 4, b += 4)
				*(unsigned long *)b = *(unsigned long *)a;
			for (; i > 0; i--)
				*b++ = *a++;
		} else
			for (i = 0; i < w; i++)
				d[i * step] = op(operation, s[i * step], d[i * step]);
	}
	return 1;
}

long CDECL c_line_draw(Virtual *vwk, long x1, long y1, long x2, long y2,
    long pattern, long colour, long mode)
{
	Workstation *wk;
	unsigned long fg, bg;
	unsigned short mask = 0x8000;
	long dx, dy, sx, sy, err, e2, wrap;
	Pix *d;

	if ((long)vwk & 1)
		return -1;
	if (!clip_line(vwk, &x1, &y1, &x2, &y2))
		return 1;
	wk = vwk->real_address;
	c_get_colours(vwk, colour, &fg, &bg);
	wrap = wk->screen.wrap;
	d = (Pix *)wk->screen.mfdb.address + y1 * wrap + x1;
	dx = x2 > x1 ? x2 - x1 : x1 - x2;
	dy = y2 > y1 ? y2 - y1 : y1 - y2;
	sx = x2 > x1 ? 1 : -1;
	sy = y2 > y1 ? wrap : -wrap;
	err = dx - dy;
	for (;;) {
		PUT(d, pattern & mask, mode, fg, bg);
		if (!(mask >>= 1))
			mask = 0x8000;
		if (x1 == x2 && y1 == y2)
			break;
		e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			x1 += sx;
			d += sx;
		}
		if (e2 < dx) {
			err += dx;
			y1 += sy > 0 ? 1 : -1;
			d += sy;
		}
	}
	return 1;
}

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
