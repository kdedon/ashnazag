/*
 * A Voodoo3 (or Banshee) behind the PCI bridge as the display service's
 * screen.  One mode at the size Space.c names, 8-bit CLUT, RGB565 or
 * 32 bits a pixel per session, each session a frame of VRAM of its own
 * while room lasts; bringing one to the front moves the desktop start.
 * Only the 2D video path is set up; the 3D engine is left as reset.
 *
 * A 3D driver for the card shares with this module, and must take from
 * it rather than map or program again:
 * - the card: this module holds it (opci_obtain) while loaded, and the
 *   register and frame buffer mappings (v3_reg, v3_fb, v3_physfb) and
 *   the register byte order (v3_swap);
 * - VRAM: sessions use [0, v3_dsvram); [v3_dsvram, v3_vram) is free for
 *   back, depth and texture buffers;
 * - ownership: the front session (am_rtg's r_pan offset) is the one a
 *   3D client may draw to the screen for; v3_fronthook, when set, is
 *   called after each change.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sys/opci.h"
#include "sys/amrtg.h"

extern int printf();
extern unsigned long plat_vtop();
extern int v3ds_width, v3ds_height;
extern unsigned long v3ds_resv;

/* registers at BAR0 */
#define	STATUS		0x00
#define	LFBMEMCFG	0x0c
#define	MISCINIT0	0x10
#define	MISCINIT1	0x14
#define	VGAINIT0	0x28
#define	PLLCTRL0	0x40
#define	DACMODE		0x4c
#define	DACADDR		0x50
#define	DACDATA		0x54
#define	VIDPROCCFG	0x5c
#define	VIDSCREENSIZE	0x98
#define	VIDDESKSTART	0xe4
#define	VIDDESKSTRIDE	0xe8
#define	VGA(p)		((p) - 0x300)	/* VGA port p in the I/O BAR */

/* vidProcCfg */
#define	VP_ON		0x00000001
#define	VP_DESKTOP	0x00000080
#define	VP_CLUTBYPASS	0x00000400
#define	VP_FMT(n)	((unsigned long)(n) << 18)	/* 0 CLUT, 1 RGB565, 3 RGB32 */

/*
 * miscInit0 bits 30-31: the frame buffer aperture reverses the bytes of
 * each long (30), then swaps its words (31), for a big-endian host.  A
 * 32-bit pixel takes 30, a 16-bit one both.
 */
#define	MI0_SWIZ(n)	((unsigned long)(n) << 30)

volatile unsigned char *v3_reg, *v3_fb;
unsigned long v3_physfb, v3_vram, v3_dsvram;
int v3_swap;			/* register longs reach the card byte reversed */
struct opci_dev *v3_dev;
void (*v3_fronthook)();

static volatile unsigned char *v3_io;
static struct am_rtg v3_rtg;
static unsigned long v3_vp;	/* vidProcCfg with the video processor on */
static int v3_bits = 8, v3_shown;

static unsigned long
swab32(v)
unsigned long v;
{
	return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24;
}

static void
wl(r, v)
int r;
unsigned long v;
{
	*(volatile unsigned long *)(v3_reg + r) = v3_swap ? swab32(v) : v;
}

static unsigned long
rl(r)
int r;
{
	unsigned long v = *(volatile unsigned long *)(v3_reg + r);

	return v3_swap ? swab32(v) : v;
}

static void
wb(p, v)
int p, v;
{
	v3_io[VGA(p)] = v;
}

static void
crtc(i, v)
int i, v;
{
	wb(0x3d4, i);
	wb(0x3d5, v);
}

/* VESA timings: pixel clock in kHz, sync start and end, total */
static struct v3tim {
	int	w, h, khz;
	int	hss, hse, ht, vss, vse, vt;
	int	misc;		/* sync polarity bits of the misc output */
} v3_tims[] = {
	{ 640, 480, 25175, 656, 752, 800, 490, 492, 525, 0xc0 },
	{ 800, 600, 40000, 840, 968, 1056, 601, 605, 628, 0x00 },
	{ 1024, 768, 65000, 1048, 1184, 1344, 771, 777, 806, 0xc0 },
	{ 1152, 864, 108000, 1216, 1344, 1600, 865, 868, 900, 0x00 },
	{ 1280, 1024, 108000, 1328, 1440, 1688, 1025, 1028, 1066, 0x00 },
};
#define	NTIM	(sizeof v3_tims / sizeof v3_tims[0])

/* pllCtrl0 for khz: 14.318 MHz * (N + 2) / ((M + 2) << K) */
static unsigned long
v3_pll(khz)
long khz;
{
	long n, m, k, f, d, best = 0x7fffffffL;
	unsigned long r = 0;

	for (k = 0; k < 4; k++)
		for (m = 0; m < 64; m++) {
			n = (khz * ((m + 2) << k) + 7159) / 14318 - 2;
			if (n < 0 || n > 255)
				continue;
			f = 14318 * (n + 2) / ((m + 2) << k);
			d = f > khz ? f - khz : khz - f;
			if (d < best) {
				best = d;
				r = n << 8 | m << 2 | k;
			}
		}
	return r;
}

static void
v3_timing(t)
struct v3tim *t;
{
	static unsigned char seq[] = { 0x03, 0x01, 0x0f, 0x00, 0x0e };
	static unsigned char gra[] = { 0, 0, 0, 0, 0, 0x40, 0x05, 0x0f, 0xff };
	int ht = t->ht / 8 - 5, hd = t->w / 8 - 1, hbe = t->ht / 8 - 1;
	int hs = t->hss / 8, he = t->hse / 8;
	int vt = t->vt - 2, vd = t->h - 1, vs = t->vss, vb = t->h, vbe = t->vt - 1;
	int i;

	wl(MISCINIT1, rl(MISCINIT1) | 1);
	wl(VIDPROCCFG, rl(VIDPROCCFG) & ~VP_ON);
	wl(PLLCTRL0, v3_pll((long)t->khz));
	wb(0x3c2, 0x0f | t->misc);
	for (i = 0; i < sizeof seq; i++) {
		wb(0x3c4, i);
		wb(0x3c5, seq[i]);
	}
	crtc(0x11, 0);
	crtc(0x00, ht);
	crtc(0x01, hd);
	crtc(0x02, hd + 1);
	crtc(0x03, 0x80 | (hbe & 0x1f));
	crtc(0x04, hs);
	crtc(0x05, (hbe & 0x20) << 2 | (he & 0x1f));
	crtc(0x06, vt);
	crtc(0x07, (vt >> 8 & 1) | (vd >> 7 & 2) | (vs >> 6 & 4) | (vb >> 5 & 8) | 0x10 |
	    (vt >> 4 & 0x20) | (vd >> 3 & 0x40) | (vs >> 2 & 0x80));
	crtc(0x08, 0);
	crtc(0x09, 0x40 | (vb >> 4 & 0x20));
	crtc(0x10, vs);
	crtc(0x11, 0x20 | ((t->vse) & 0x0f));
	crtc(0x12, vd);
	crtc(0x13, t->w / 8);
	crtc(0x14, 0);
	crtc(0x15, vb);
	crtc(0x16, vbe);
	crtc(0x17, 0xc3);
	crtc(0x18, 0xff);
	/* the bits past 8 of the horizontal and 10 of the vertical counts */
	crtc(0x1a, (ht >> 8 & 1) | (hd >> 6 & 4) | ((hd + 1) >> 4 & 0x10) | (hbe & 0x40) >> 1 |
	    (hs >> 2 & 0x40) | (he & 0x20) << 2);
	crtc(0x1b, (vt >> 10 & 1) | (vd >> 8 & 4) | (vb >> 6 & 0x10) | (vs >> 4 & 0x40));
	for (i = 0; i < sizeof gra; i++) {
		wb(0x3ce, i);
		wb(0x3cf, gra[i]);
	}
	(void)v3_io[VGA(0x3da)];
	for (i = 0; i < 0x15; i++) {
		wb(0x3c0, i);
		wb(0x3c0, i < 0x10 ? i : i == 0x10 ? 0x41 : i == 0x12 ? 0x0f : 0);
	}
	wb(0x3c0, 0x20);
	/* 8-bit DAC, extensions on, extended shift out */
	wl(VGAINIT0, 1 << 2 | 1 << 6 | 1 << 8 | 1 << 10 | 1 << 12);
	wl(DACMODE, 0);
	wl(VIDSCREENSIZE, t->w | (unsigned long)t->h << 12);
}

static void
v3_clut(i, r, g, b)
int i, r, g, b;
{
	wl(DACADDR, i);
	wl(DACDATA, (unsigned long)(r & 0xff) << 16 | (g & 0xff) << 8 | (b & 0xff));
}

static void
v3_depth(bits)
int bits;
{
	v3_bits = bits;
	/* the CPU's pixels land in the card's byte order */
	wl(MISCINIT0, (rl(MISCINIT0) & ~MI0_SWIZ(3)) | MI0_SWIZ(bits == 32 ? 1 : bits == 16 ? 3 : 0));
	wl(VIDDESKSTRIDE, (unsigned long)v3ds_width * (bits / 8));
	v3_vp = VP_ON | VP_DESKTOP |
	    (bits == 32 ? VP_FMT(3) | VP_CLUTBYPASS : bits == 16 ? VP_FMT(1) | VP_CLUTBYPASS : 0);
	if (v3_shown)
		wl(VIDPROCCFG, v3_vp);
}

static void
v3_pan(off)
unsigned long off;
{
	wl(VIDDESKSTART, off);
	if (v3_fronthook)
		(*v3_fronthook)();
}

static void
v3_show(on)
int on;
{
	v3_shown = on;
	wl(VIDPROCCFG, on ? v3_vp : v3_vp & ~(VP_ON | VP_DESKTOP));
	/* hidden, the syncs stop: a monitor shared with the chipset switches back */
	crtc(0x17, on ? 0xc3 : 0x43);
}

/* VRAM: the largest power of two up to the aperture that does not wrap */
static unsigned long
v3_sizevram(aperture)
unsigned long aperture;
{
	volatile unsigned long *p0 = (volatile unsigned long *)v3_fb, *p;
	unsigned long n = 0x200000, keep0;

	keep0 = *p0;
	while (n < aperture) {
		p = (volatile unsigned long *)(v3_fb + n);
		*p0 = 0x5a5aa5a5;
		*p = 0x12345678;
		if (*p != 0x12345678 || *p0 != 0x5a5aa5a5)
			break;
		n <<= 1;
	}
	*p0 = keep0;
	return n;
}

/* the card found, its mode set, the display service told; 0 or an errno */
int
v3ds_attach()
{
	struct opci_dev *d;
	struct v3tim *t;
	unsigned long frame;
	int i, e;

	for (t = v3_tims; t < v3_tims + NTIM; t++)
		if (t->w == v3ds_width && t->h == v3ds_height)
			break;
	if (t == v3_tims + NTIM) {
		printf("voodoo3: no timing for %dx%d\n", v3ds_width, v3ds_height);
		return EINVAL;
	}
	if (am_rtgbusy())
		return EBUSY;
	if ((d = opci_find((struct opci_dev *)0, 0x121a, 0x0005)) == 0 &&
	    (d = opci_find((struct opci_dev *)0, 0x121a, 0x0003)) == 0)
		return ENXIO;
	if (opci_obtain(d) != 0)
		return EBUSY;
	opci_cfgwrite(d, OPCI_CFG_COMMAND, 2,
	    opci_cfgread(d, OPCI_CFG_COMMAND, 2) | OPCI_CMD_IO | OPCI_CMD_MEMORY);
	v3_reg = (unsigned char *)opci_attr(d, OPCI_BARADDR(0));
	v3_fb = (unsigned char *)opci_attr(d, OPCI_BARADDR(1));
	v3_io = (unsigned char *)opci_attr(d, OPCI_BARADDR(2));
	if (v3_reg == 0 || v3_fb == 0 || v3_io == 0) {
		opci_release(d);
		return ENXIO;
	}
	/* an idle card's free FIFO count is in the low byte of status */
	v3_swap = (*(volatile unsigned long *)(v3_reg + STATUS) & 0x1f) == 0;
	/* tiled aperture at the top: the frame buffer window is linear */
	wl(LFBMEMCFG, 0x1fff);
	wl(MISCINIT0, rl(MISCINIT0) & ~MI0_SWIZ(3));
	v3_vram = v3_sizevram(opci_attr(d, OPCI_BARSIZE(1)));
	frame = ((unsigned long)v3ds_width * v3ds_height * 4 + 4095) & ~4095UL;
	v3_dsvram = v3_vram > v3ds_resv + frame ? v3_vram - v3ds_resv : frame;
	if (frame > v3_vram) {
		opci_release(d);
		return ENOMEM;
	}
	v3_physfb = plat_vtop((unsigned long)v3_fb);
	v3_dev = d;
	v3_timing(t);
	v3_pan(0L);
	v3_depth(8);
	for (i = 0; i < 256; i++)
		v3_clut(i, i, i, i);
	v3_show(0);
	v3_rtg.r_base = (unsigned long)v3_fb;
	v3_rtg.r_phys = v3_physfb;
	v3_rtg.r_width = v3ds_width;
	v3_rtg.r_height = v3ds_height;
	v3_rtg.r_depth = 8;
	v3_rtg.r_row = v3ds_width;
	v3_rtg.r_name = "Voodoo3";
	v3_rtg.r_clut = v3_clut;
	v3_rtg.r_show = v3_show;
	v3_rtg.r_depths = 1 | 2 | 4;
	v3_rtg.r_rgb[0][0] = 0xf800;
	v3_rtg.r_rgb[0][1] = 0x07e0;
	v3_rtg.r_rgb[0][2] = 0x001f;
	v3_rtg.r_rgb[1][0] = 0x00ff0000;
	v3_rtg.r_rgb[1][1] = 0x0000ff00;
	v3_rtg.r_rgb[1][2] = 0x000000ff;
	v3_rtg.r_setdepth = v3_depth;
	v3_rtg.r_pan = v3_pan;
	v3_rtg.r_vram = v3_dsvram;
	printf("voodoo3: %dx%d, %d MB, %d frames, registers %s\n", v3ds_width, v3ds_height,
	    (int)(v3_vram >> 20), (int)(v3_dsvram / frame), v3_swap ? "byte swapped" : "in order");
	if ((e = am_rtgattach(&v3_rtg)) != 0) {
		v3_dev = 0;
		opci_release(d);
	}
	return e;
}

int
v3ds_detach()
{
	int e;

	if ((e = am_rtgdetach(&v3_rtg)) != 0)
		return e;
	if (v3_dev) {
		v3_show(0);
		opci_release(v3_dev);
		v3_dev = 0;
	}
	return 0;
}
