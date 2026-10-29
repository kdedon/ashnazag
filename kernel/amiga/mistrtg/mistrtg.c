/*
 * The MiSTer Minimig core's Zorro III RTG board as the display service's
 * screen.  The core's scaler makes the timing: the card takes any size,
 * and showing it is a register; there is no interrupt, cursor or blitter.
 * Disabled, the chipset's picture is shown.
 */
#include "sys/types.h"
#include "sys/errno.h"
#include "sys/amrtg.h"
#include "amilib.h"

#define	MR_MANUF	0x139C
#define	MR_PROD		0x30
#define	MR_FBSIZE	0x800000UL	/* the framebuffer, at the board's base */
#define	MR_REGS		0x800100UL
#define	MR_MAP		(MR_REGS + 0x700)	/* through the CLUT */
#define	MR_FBADDR	0x27000000UL	/* the framebuffer as the scaler addresses it */
#define	MR_ID		0x5001

/* registers, 16 bits each */
#define	R_ADDRHI	0x0
#define	R_ADDRLO	0x2
#define	R_FORMAT	0x4
#define	R_ENABLE	0x6
#define	R_HSIZE		0x8
#define	R_VSIZE		0xA
#define	R_STRIDE	0xC
#define	R_IDREG		0xE
#define	R_CLUT		0x300		/* 256 x 00RR, GGBB */

#define	F_CLUT		3
#define	F_565		4
#define	F_RGB32		6

#define	REG(off)	(*(__volatile__ unsigned short *)(mr_regs + (off)))

extern int plat_zorro(), printf();
extern char *plat_iomap();
extern int mr_width, mr_height;

static char *mr_board, *mr_regs;
static struct am_rtg mr_rtg;
static int mr_w, mr_h, mr_bits;

static struct am_mode mr_modes[] = {
	{ 640, 480, 60 }, { 800, 600, 60 }, { 1024, 768, 60 }, { 1280, 720, 60 },
	{ 1280, 1024, 60 }, { 1600, 1200, 60 }, { 1920, 1080, 60 }
};
#define	NMODE	(sizeof mr_modes / sizeof mr_modes[0])

static void
mr_clut(i, r, g, b)
int i, r, g, b;
{
	REG(R_CLUT + i * 4) = r & 0xff;
	REG(R_CLUT + i * 4 + 2) = (g & 0xff) << 8 | (b & 0xff);
}

static void
mr_load()
{
	REG(R_FORMAT) = mr_bits == 32 ? F_RGB32 : mr_bits == 16 ? F_565 : F_CLUT;
	REG(R_HSIZE) = mr_w;
	REG(R_VSIZE) = mr_h;
	REG(R_STRIDE) = mr_w * (mr_bits / 8);
	REG(R_ADDRHI) = MR_FBADDR >> 16;
	REG(R_ADDRLO) = 0;
}

static void
mr_depth(bits)
int bits;
{
	mr_bits = bits;
	mr_load();
}

static void
mr_setmode(n, bits)
int n, bits;
{
	mr_w = mr_modes[n].m_width;
	mr_h = mr_modes[n].m_height;
	mr_depth(bits);
}

static void
mr_show(on)
int on;
{
	REG(R_ENABLE) = on ? 1 : 0;
}

/* the board found, its mode set, the display service told; 0 or an errno */
int
mr_attach()
{
	struct amx_zboard zb;
	unsigned long px;
	int i, e;

	for (i = 0; i < NMODE; i++)
		if (mr_modes[i].m_width == mr_width && mr_modes[i].m_height == mr_height)
			break;
	if (i == NMODE) {
		printf("mistrtg: no mode %dx%d\n", mr_width, mr_height);
		return EINVAL;
	}
	if (am_rtgbusy())
		return EBUSY;
	for (e = 0; plat_zorro(e, &zb) == 0; e++)
		if (zb.zb_manuf == MR_MANUF && zb.zb_prod == MR_PROD && zb.zb_size > MR_MAP)
			break;
	if (plat_zorro(e, &zb) != 0)
		return ENXIO;
	/* the board stays mapped: its window costs no kernel space */
	if ((mr_board = plat_iomap(zb.zb_pa, MR_MAP)) == 0)
		return ENOMEM;
	mr_regs = mr_board + MR_REGS;
	if (REG(R_IDREG) != MR_ID) {
		printf("mistrtg: id %x\n", REG(R_IDREG));
		return ENXIO;
	}
	mr_show(0);
	mr_setmode(i, 8);
	for (e = 0; e < 256; e++)
		mr_clut(e, e, e, e);
	px = (unsigned long)mr_width * mr_height;
	mr_rtg.r_base = (unsigned long)mr_board;
	mr_rtg.r_phys = zb.zb_pa;
	mr_rtg.r_width = mr_width;
	mr_rtg.r_height = mr_height;
	mr_rtg.r_depth = 8;
	mr_rtg.r_row = mr_width;
	mr_rtg.r_name = "MiSTer RTG";
	mr_rtg.r_clut = mr_clut;
	mr_rtg.r_show = mr_show;
	/* the service's buffer: the deepest frame and a page */
	mr_rtg.r_depths = 1;
	if (px * 2 + 4096 <= MR_FBSIZE)
		mr_rtg.r_depths |= 2;
	if (px * 4 + 4096 <= MR_FBSIZE)
		mr_rtg.r_depths |= 4;
	mr_rtg.r_rgb[0][0] = 0xf800;
	mr_rtg.r_rgb[0][1] = 0x07e0;
	mr_rtg.r_rgb[0][2] = 0x001f;
	mr_rtg.r_rgb[1][0] = 0x00ff0000;
	mr_rtg.r_rgb[1][1] = 0x0000ff00;
	mr_rtg.r_rgb[1][2] = 0x000000ff;
	mr_rtg.r_setdepth = mr_depth;
	mr_rtg.r_modes = mr_modes;
	mr_rtg.r_nmode = NMODE;
	mr_rtg.r_mode = i;
	mr_rtg.r_setmode = mr_setmode;
	printf("mistrtg: %dx%d at %x\n", mr_width, mr_height, (int)zb.zb_pa);
	return am_rtgattach(&mr_rtg);
}

int
mr_detach()
{
	int e;

	if ((e = am_rtgdetach(&mr_rtg)) != 0)
		return e;
	if (mr_regs)
		mr_show(0);
	return 0;
}
