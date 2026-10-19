/*
 * A Voodoo3 (or Banshee) behind the PCI bridge as the display service's
 * screen.  Its VESA modes up to the size Space.c names, 8-bit CLUT,
 * RGB565 or 32 bits a pixel per session.  The display service hands out VRAM:
 * the front session's frame at its start, up to v3ds_altfb frames that
 * hidden sessions keep; bringing one of those to the front moves the
 * desktop start.
 * Only the 2D video path is set up; the 3D engine is left as reset.
 *
 * A 3D driver for the card shares with this module, and must take from
 * it rather than map or program again:
 * - the card: this module holds it (opci_obtain) while loaded, and the
 *   register and frame buffer mappings (v3_reg, v3_fb, v3_physfb) and
 *   the register byte order (v3_swap);
 * - VRAM: the command fifo, back, depth and texture buffers go where
 *   am_vtake() says, from its first open to its last close;
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
extern void drv_usecwait();
extern unsigned long plat_vtop();
extern int v3ds_width, v3ds_height;
extern int v3ds_altfb;

/* registers at BAR0 */
#define	STATUS		0x00
#define	LFBMEMCFG	0x0c
#define	MISCINIT0	0x10
#define	MISCINIT1	0x14
#define	DRAMINIT0	0x18
#define	DRAMINIT1	0x1c
#define	VGAINIT0	0x28
#define	DRAMCOMMAND	0x30
#define	DRAMDATA	0x34
#define	PLLCTRL0	0x40
#define	PLLCTRL1	0x44
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

__volatile__ unsigned char *v3_reg, *v3_fb;
unsigned long v3_physfb, v3_vram, v3_dsvram;
int v3_swap;			/* register longs reach the card byte reversed */
struct opci_dev *v3_dev;
void (*v3_fronthook)();

static __volatile__ unsigned char *v3_io;
static struct am_rtg v3_rtg;
static unsigned long v3_vp;	/* vidProcCfg with the video processor on */
static int v3_bits = 8, v3_shown, v3_w;
static struct am_mode v3_modes[5];

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
	*(__volatile__ unsigned long *)(v3_reg + r) = v3_swap ? swab32(v) : v;
}

static unsigned long
rl(r)
int r;
{
	unsigned long v = *(__volatile__ unsigned long *)(v3_reg + r);

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
static struct v3tim *v3_tim;

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
	v3_w = t->w;
	v3_tim = t;
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
	wl(VIDDESKSTRIDE, (unsigned long)v3_w * (bits / 8));
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

/* timing n at bits: blanked, set in the vertical blank, shown again */
static void
v3_setmode(n, bits)
int n, bits;
{
	int i;

	if (&v3_tims[n] != v3_tim) {
		if (v3_shown) {
			for (i = 0; i < 100000 && !(v3_io[VGA(0x3da)] & 8); i++)
				;
			wl(VIDPROCCFG, v3_vp & ~(VP_ON | VP_DESKTOP));
		}
		v3_timing(&v3_tims[n]);
		crtc(0x17, v3_shown ? 0xc3 : 0x43);
	}
	v3_depth(bits);
}

/* board straps: dramInit0 SGRAM part size and count, dramInit1 SDRAM, miscInit1 PCI */
#define	DI0_STRAPS	0x0c000000
#define	DI1_SDRAM	0x40000000
#define	MI1_STRAPS	0xff000000
#define	PLL_RESET	0xc01		/* pllCtrl1 at power on: 50 MHz */
#define	CFG_ROM		0x30

/* the frame buffer wraps every 1 MB until the SGRAM is set up */
static int
v3_wraps()
{
	__volatile__ unsigned long *p0 = (__volatile__ unsigned long *)v3_fb;
	__volatile__ unsigned long *p1 = (__volatile__ unsigned long *)(v3_fb + 0x100000);
	unsigned long k0 = *p0, k1 = *p1;
	int w;

	*p0 = 0x5a5aa5a5;
	*p1 = 0x12345678;
	w = *p0 != 0x5a5aa5a5;
	*p1 = k1;
	*p0 = k0;
	return w;
}

static unsigned long
romw(rom, o, n)
__volatile__ unsigned char *rom;
unsigned long o;
int n;
{
	unsigned long v = 0;

	while (n-- > 0)
		v = v << 8 | rom[o + n];
	return v;
}

/*
 * The card's BIOS init table: ROM word 0x50 points at a word that points
 * at nine longs, pciInit0, miscInit0, miscInit1, dramInit0, dramInit1,
 * agpInit, pllCtrl1, pllCtrl2 and the SGRAM mode.  0 if there is none.
 */
static int
v3_romtab(d, tab)
struct opci_dev *d;
unsigned long *tab;
{
	__volatile__ unsigned char *rom = (unsigned char *)opci_attr(d, OPCI_ROMADDR);
	unsigned long size = opci_attr(d, OPCI_ROMSIZE), cfg, o;
	int i, ok = 0;

	if (rom == 0 || size < 0x100)
		return 0;
	cfg = opci_cfgread(d, CFG_ROM, 4);
	opci_cfgwrite(d, CFG_ROM, 4, cfg | 1);
	if (rom[0] == 0x55 && rom[1] == 0xaa &&
	    (o = romw(rom, 0x50L, 2)) + 2 <= size && (o = romw(rom, o, 2)) + 36 <= size) {
		for (i = 0; i < 9; i++)
			tab[i] = romw(rom, o + i * 4, 4);
		ok = tab[6] != 0 && tab[6] <= 0xffff;
	}
	opci_cfgwrite(d, CFG_ROM, 4, cfg);
	return ok;
}

/*
 * Set up the memory as a PC BIOS does at power on: memory clock and
 * timings from the ROM's table (else generic 16 Mbit SGRAM timings at
 * 143 MHz), then load the SGRAM mode register, write mask and colour.
 */
static void
v3_meminit(d)
struct opci_dev *d;
{
	unsigned long tab[9], di0, di1;
	int rom = v3_romtab(d, tab);

	if (!rom) {
		tab[2] = rl(MISCINIT1) | 1;
		tab[3] = 0x00179d29;
		tab[4] = 0x0028c031;
		tab[6] = 0x3a05;
	}
	if (!rom || tab[8] > 0xfff)
		tab[8] = 0x37;
	di0 = (tab[3] & ~DI0_STRAPS) | (rl(DRAMINIT0) & DI0_STRAPS);
	di1 = (tab[4] & ~DI1_SDRAM) | (rl(DRAMINIT1) & DI1_SDRAM);
	wl(PLLCTRL1, tab[6]);
	drv_usecwait(10000);
	wl(DRAMINIT1, di1);
	wl(DRAMINIT0, di0);
	wl(MISCINIT1, (tab[2] & ~MI1_STRAPS) | (rl(MISCINIT1) & MI1_STRAPS));
	drv_usecwait(1000);
	wl(DRAMDATA, tab[8]);
	wl(DRAMCOMMAND, 0x10dL);
	drv_usecwait(1000);
	if (!(di1 & DI1_SDRAM)) {
		wl(DRAMDATA, 0xffffffffL);
		wl(DRAMCOMMAND, 0x10eL);
		drv_usecwait(1000);
		wl(DRAMDATA, 0L);
		wl(DRAMCOMMAND, 0x10fL);
		drv_usecwait(1000);
	}
	printf("voodoo3: memory set up %s, clock %x, dramInit %x %x\n",
	    rom ? "from the ROM table" : "with defaults", (int)tab[6], (int)di0, (int)di1);
}

/* VRAM: the largest power of two up to the aperture that does not wrap */
static unsigned long
v3_sizevram(aperture)
unsigned long aperture;
{
	__volatile__ unsigned long *p0 = (__volatile__ unsigned long *)v3_fb, *p;
	unsigned long n = 0x200000, keep0;

	keep0 = *p0;
	*p0 = 0x5a5aa5a5;
	if (*p0 != 0x5a5aa5a5)
		return 0;
	while (n < aperture) {
		p = (__volatile__ unsigned long *)(v3_fb + n);
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
	unsigned long frame, n;
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
	v3_swap = (*(__volatile__ unsigned long *)(v3_reg + STATUS) & 0x1f) == 0;
	/* tiled aperture at the top: the frame buffer window is linear */
	wl(LFBMEMCFG, 0x1fff);
	wl(MISCINIT0, rl(MISCINIT0) & ~MI0_SWIZ(3));
	if (rl(PLLCTRL1) == PLL_RESET || v3_wraps())
		v3_meminit(d);
	v3_vram = v3_sizevram(opci_attr(d, OPCI_BARSIZE(1)));
	if (v3_vram == 0) {
		printf("voodoo3: card not responding at %x\n", (int)plat_vtop((unsigned long)v3_fb));
		opci_release(d);
		return ENXIO;
	}
	/* a 32-bit screen and a page: Picasso96 keeps bytes between two 16-bit ones */
	frame = (((unsigned long)v3ds_width * v3ds_height * 4 + 4095) & ~4095UL) + 4096;
	if (frame > v3_vram) {
		printf("voodoo3: %d KB VRAM, %dx%d needs %d KB\n", (int)(v3_vram >> 10),
		    v3ds_width, v3ds_height, (int)(frame >> 10));
		opci_release(d);
		return ENOMEM;
	}
	n = v3_vram / frame - 1;
	if (v3ds_altfb >= 0 && v3ds_altfb < n)
		n = v3ds_altfb;
	v3_dsvram = (n + 1) * frame;
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
	for (i = 0; i < NTIM; i++) {
		v3_modes[i].m_width = v3_tims[i].w;
		v3_modes[i].m_height = v3_tims[i].h;
		v3_modes[i].m_hz = (int)(v3_tims[i].khz * 1000L / ((long)v3_tims[i].ht * v3_tims[i].vt));
	}
	v3_rtg.r_modes = v3_modes;
	v3_rtg.r_nmode = NTIM;
	v3_rtg.r_mode = t - v3_tims;
	v3_rtg.r_setmode = v3_setmode;
	v3_rtg.r_pan = v3_pan;
	v3_rtg.r_vram = v3_vram;
	v3_rtg.r_altfb = n;
	printf("voodoo3: %dx%d, %d MB, %d frames, %d KB for 3D, registers %s\n", v3ds_width,
	    v3ds_height, (int)(v3_vram >> 20), (int)(v3_dsvram / frame),
	    (int)((v3_vram - v3_dsvram) >> 10), v3_swap ? "byte swapped" : "in order");
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
