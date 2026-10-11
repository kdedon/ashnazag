/*
 * v3bars -- loading it finds a Voodoo3 (or Banshee) behind the PCI
 * bridge, prints its ID and status registers, sets 640x480 in RGB565
 * and fills the screen with colour bars.  A bring-up check for the
 * card; it holds nothing once loaded.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sys/moddefs.h"
#include "sys/opci.h"

extern int printf();

#define	W	640
#define	H	480

/* registers at BAR0 */
#define	STATUS		0x00
#define	PCIINIT0	0x04
#define	LFBMEMCFG	0x0c
#define	MISCINIT0	0x10
#define	MISCINIT1	0x14
#define	DRAMINIT0	0x18
#define	VGAINIT0	0x28
#define	VGAINIT1	0x2c
#define	PLLCTRL0	0x40
#define	DACMODE		0x4c
#define	VIDPROCCFG	0x5c
#define	VIDSCREENSIZE	0x98
#define	VIDDESKSTART	0xe4
#define	VIDDESKSTRIDE	0xe8
#define	VGA(p)		((p) - 0x300)	/* VGA port p in the I/O BAR */

static volatile unsigned char *v3reg, *v3io;
static int v3swap;		/* longs reach the card byte reversed */

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
	*(volatile unsigned long *)(v3reg + r) = v3swap ? swab32(v) : v;
}

static unsigned long
rl(r)
int r;
{
	unsigned long v = *(volatile unsigned long *)(v3reg + r);

	return v3swap ? swab32(v) : v;
}

static void
wb(p, v)
int p, v;
{
	v3io[VGA(p)] = v;
}

static int
rb(p)
int p;
{
	return v3io[VGA(p)];
}

/* 640x480 at 60 Hz: 800x525 total, 25.175 MHz */
static unsigned char crt[] = {
	0x5f, 0x4f, 0x4f, 0x83, 0x51, 0x9d, 0x0b, 0x3e,
	0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xe9, 0x2b, 0xdf, 0x50, 0x00, 0xdf, 0x0c, 0xc3, 0xff
};
static unsigned char seq[] = { 0x03, 0x01, 0x0f, 0x00, 0x0e };
static unsigned char gra[] = { 0, 0, 0, 0, 0, 0x40, 0x05, 0x0f, 0xff };

static void
setmode()
{
	int i;

	wl(MISCINIT1, rl(MISCINIT1) | 1);
	wl(VIDPROCCFG, rl(VIDPROCCFG) & ~1);
	/* N 56, M 31, K 0: 14.318 MHz * 58 / 33 */
	wl(PLLCTRL0, 56 << 8 | 31 << 2);
	wb(0x3c2, 0xcf);
	for (i = 0; i < sizeof seq; i++) {
		wb(0x3c4, i);
		wb(0x3c5, seq[i]);
	}
	wb(0x3d4, 0x11);
	wb(0x3d5, 0x2b);
	for (i = 0; i < sizeof crt; i++) {
		wb(0x3d4, i);
		wb(0x3d5, crt[i]);
	}
	wb(0x3d4, 0x1a);
	wb(0x3d5, 0);
	wb(0x3d4, 0x1b);
	wb(0x3d5, 0);
	for (i = 0; i < sizeof gra; i++) {
		wb(0x3ce, i);
		wb(0x3cf, gra[i]);
	}
	(void)rb(0x3da);
	for (i = 0; i < 0x15; i++) {
		wb(0x3c0, i);
		wb(0x3c0, i < 0x10 ? i : i == 0x10 ? 0x41 : i == 0x12 ? 0x0f : 0);
	}
	wb(0x3c0, 0x20);
	/* 8-bit DAC, extensions on, extended shift out */
	wl(VGAINIT0, 1 << 2 | 1 << 6 | 1 << 8 | 1 << 10 | 1 << 12);
	wl(DACMODE, 0);
	wl(VIDDESKSTRIDE, W * 2);
	wl(VIDSCREENSIZE, W | H << 12);
	wl(VIDDESKSTART, 0);
	/* video processor and desktop on, 16 bpp, CLUT bypassed */
	wl(VIDPROCCFG, 1 | 1 << 7 | 1 << 18 | 1 << 10);
}

static unsigned short bars[] = {
	0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0x0000
};

static void
draw(fb)
volatile unsigned short *fb;
{
	int x, y;
	unsigned short c;

	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			if (y < H * 3 / 4)
				c = bars[x * 8 / W];
			else		/* grey ramp */
				c = (x * 32 / W) << 11 | (x * 64 / W) << 5 | x * 32 / W;
			*fb++ = c >> 8 | c << 8;
		}
}

static int
v3bars_load()
{
	struct opci_dev *d;
	volatile unsigned short *fb;
	unsigned long id;

	if ((d = opci_find((struct opci_dev *)0, 0x121a, 0x0005)) == 0 &&
	    (d = opci_find((struct opci_dev *)0, 0x121a, 0x0003)) == 0) {
		printf("v3bars: no Voodoo3 or Banshee\n");
		return ENXIO;
	}
	if (opci_obtain(d) != 0) {
		printf("v3bars: card busy\n");
		return EBUSY;
	}
	opci_cfgwrite(d, OPCI_CFG_COMMAND, 2,
	    opci_cfgread(d, OPCI_CFG_COMMAND, 2) | OPCI_CMD_IO | OPCI_CMD_MEMORY);
	id = opci_cfgread(d, OPCI_CFG_VENDOR, 4);
	v3reg = (unsigned char *)opci_attr(d, OPCI_BARADDR(0));
	fb = (unsigned short *)opci_attr(d, OPCI_BARADDR(1));
	v3io = (unsigned char *)opci_attr(d, OPCI_BARADDR(2));
	printf("v3bars: %x:%x rev %x, regs %x (%x), fb %x (%x), io %x\n",
	    (int)id & 0xffff, (int)(id >> 16),
	    (int)opci_cfgread(d, OPCI_CFG_CLASSREV, 1), (int)v3reg,
	    (int)opci_attr(d, OPCI_BARSIZE(0)), (int)fb,
	    (int)opci_attr(d, OPCI_BARSIZE(1)), (int)v3io);
	/* an idle card's free FIFO count is in the low byte of status */
	v3swap = (*(volatile unsigned long *)(v3reg + STATUS) & 0x1f) == 0;
	printf("v3bars: status %x pciInit0 %x miscInit0 %x dramInit0 %x, %s\n",
	    (int)rl(STATUS), (int)rl(PCIINIT0), (int)rl(MISCINIT0),
	    (int)rl(DRAMINIT0),
	    v3swap ? "byte swapped" : "in order");
	/* tiled aperture at the top: the frame buffer window is linear */
	wl(LFBMEMCFG, 0x1fff);
	draw(fb);
	printf("v3bars: bars drawn, setting the mode\n");
	setmode();
	wb(0x3d4, 0x17);
	printf("v3bars: vidProcCfg %x screen %x stride %x CR17 %x\n",
	    (int)rl(VIDPROCCFG), (int)rl(VIDSCREENSIZE), (int)rl(VIDDESKSTRIDE),
	    rb(0x3d5));
	opci_release(d);
	return 0;
}

static int
v3bars_unload()
{
	return 0;
}

MOD_MISC_WRAPPER(v3bars, v3bars_load, v3bars_unload, "Voodoo3 colour bars");
