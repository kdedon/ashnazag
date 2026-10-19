/*
 * The radeon DRM as a loadable driver: /dev/dri/card1 (major 66) over
 * the card the radeonds module set up for the display service.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/errno.h"
#include "sys/moddefs.h"
#include "sys/opci.h"
#include "sys/amrtg.h"

#include "drmcard.h"

extern int radeon_svr4open(), radeon_svr4close(), radeon_svr4read(), radeon_svr4write();
extern int radeon_svr4ioctl(), radeon_svr4mmap(), spec_segmap();
extern int radeon_amix_attach(), radeon_amix_detach();
extern void radeon_amix_retarget(), rds_tell();
extern unsigned long plat_vtop();
extern __volatile__ unsigned char *rd_reg, *rd_fb;
extern unsigned long rd_physfb, rd_vram;
extern int rds_width, rds_height;
extern struct opci_dev *rd_dev;
extern void (*rd_fronthook)();
static int radeon_flag;

struct mod_drv_data radeon_drvdata[] = {
	{ { 0 }, 0, 0,
	  { radeon_svr4open, radeon_svr4close, radeon_svr4read, radeon_svr4write, radeon_svr4ioctl,
	    radeon_svr4mmap, spec_segmap, 0, 0, 0, 0, 0, &radeon_flag }, 66, 1 }
};

static int
radeon_load()
{
	struct drm_amixcard c;
	int e;

	if (rd_dev == 0 || rd_reg == 0)
		return ENXIO;
	c.c_reg = rd_reg;
	c.c_physreg = plat_vtop((unsigned long)rd_reg);
	c.c_regsize = opci_attr(rd_dev, OPCI_BARSIZE(2));
	c.c_fb = rd_fb;
	c.c_physfb = rd_physfb;
	c.c_fbsize = rd_vram;
	c.c_vram = rd_vram;
	c.c_free = 0;
	c.c_vtake = am_vtake;
	c.c_width = rds_width;
	c.c_height = rds_height;
	c.c_dev = rd_dev;
	if ((e = radeon_amix_attach(&c)) != 0)
		return e;
	rd_fronthook = radeon_amix_retarget;
	rds_tell();
	return 0;
}

static int
radeon_unload()
{
	int e;

	if ((e = radeon_amix_detach()) != 0)
		return e;
	rd_fronthook = 0;
	return 0;
}

MOD_DRV_WRAPPER(radeon, radeon_load, radeon_unload, 0, "Radeon DRM");
