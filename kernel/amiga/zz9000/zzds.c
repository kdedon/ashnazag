/*
 * The ZZ9000 as the display service's screen: an 8-bit CLUT mode at the
 * size Space.c names, shown for user sessions; the console stays on the
 * chipset, which the card passes through.
 */
#include "sys/types.h"
#include "sys/errno.h"
#include "sys/amrtg.h"
#include "zz9000.h"

#define REG(off)	(*(volatile unsigned short *)(zzds_base + (off)))

extern int autocon(), printf();
extern int zzds_mode, zzds_width, zzds_height;

static unsigned long zzds_base;
static struct am_rtg zzds_rtg;

static void
zzds_clut(i, r, g, b)
int i, r, g, b;
{
	REG(ZZ9K_REG_CX_DATA_HI) = i << 8 | (r & 0xff);
	REG(ZZ9K_REG_CX_DATA_LO) = (g & 0xff) << 8 | (b & 0xff);
	REG(ZZ9K_REG_CX_OP) = 3;
}

static void
zzds_pan(off)
unsigned long off;
{
	/* the card takes the origin and row width from the blitter registers */
	REG(ZZ9K_REG_BLITTER_X1) = 0;
	REG(ZZ9K_REG_BLITTER_Y1) = 0;
	REG(ZZ9K_REG_BLITTER_X2) = zzds_width;
	REG(ZZ9K_REG_BLITTER_COLORMODE) = ZZ9K_COLOR_8BIT;
	REG(ZZ9K_REG_PAN_HI) = off >> 16;
	REG(ZZ9K_REG_PAN_LO) = off;
}

static void
zzds_show(on)
int on;
{
	if (on) {
		REG(ZZ9K_REG_CX_CAPTURE) = 0;
		zzds_pan(0L);
	} else {
		zzds_pan(ZZ9K_CAPTURE_PAN);
		REG(ZZ9K_REG_CX_CAPTURE) = 1;
	}
}

/* the board, its mode, and the display service told; 0 or an errno */
int
zzds_attach()
{
	long base, size;

	if (!autocon(ZZ9K_PRODUCT_PC_Z2, 0, &base, &size))
		return ENXIO;
	if ((unsigned long)zzds_width * zzds_height >
	    (unsigned long)size - ZZ9K_FB_OFFSET - ZZ9K_Z2_RESERVED_TAIL)
		return ENOMEM;
	zzds_base = base;
	REG(ZZ9K_REG_BLITTER_COLORMODE) = ZZ9K_COLOR_8BIT;
	REG(ZZ9K_REG_MODE) = zzds_mode | ZZ9K_COLOR_8BIT << 8;
	zzds_show(0);
	zzds_rtg.r_base = base + ZZ9K_FB_OFFSET;
	zzds_rtg.r_width = zzds_width;
	zzds_rtg.r_height = zzds_height;
	zzds_rtg.r_depth = 8;
	zzds_rtg.r_row = zzds_width;
	zzds_rtg.r_name = "ZZ9000";
	zzds_rtg.r_clut = zzds_clut;
	zzds_rtg.r_show = zzds_show;
	return am_rtgattach(&zzds_rtg);
}

int
zzds_detach()
{
	return am_rtgdetach(&zzds_rtg);
}
