/*
 * The tdfx DRM as a loadable driver: /dev/dri/card0 (major 63) over
 * the card the voodoo3 module set up for the display service.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/errno.h"
#include "sys/moddefs.h"
#include "sys/opci.h"

#include "drmcard.h"

extern int tdfx_svr4open(), tdfx_svr4close(), tdfx_svr4read(), tdfx_svr4write();
extern int tdfx_svr4ioctl(), tdfx_svr4mmap(), spec_segmap();
extern int tdfx_amix_attach(), tdfx_amix_detach();
extern void tdfx_amix_front();
extern unsigned long plat_vtop();
extern __volatile__ unsigned char *v3_reg, *v3_fb;
extern unsigned long v3_physfb, v3_vram, v3_dsvram;
extern int v3ds_width, v3ds_height;
extern struct opci_dev *v3_dev;
extern void (*v3_fronthook)();
static int tdfx_flag;

struct mod_drv_data tdfx_drvdata[] = {
	{ { 0 }, 0, 0,
	  { tdfx_svr4open, tdfx_svr4close, tdfx_svr4read, tdfx_svr4write, tdfx_svr4ioctl,
	    tdfx_svr4mmap, spec_segmap, 0, 0, 0, 0, 0, &tdfx_flag }, 63, 1 }
};

static int
tdfx_load()
{
	struct drm_amixcard c;
	int e;

	if (v3_dev == 0 || v3_reg == 0)
		return ENXIO;
	c.c_reg = v3_reg;
	c.c_physreg = plat_vtop((unsigned long)v3_reg);
	c.c_regsize = opci_attr(v3_dev, OPCI_BARSIZE(0));
	c.c_fb = v3_fb;
	c.c_physfb = v3_physfb;
	c.c_fbsize = v3_vram;
	c.c_vram = v3_vram;
	c.c_free = v3_dsvram;
	c.c_width = v3ds_width;
	c.c_height = v3ds_height;
	c.c_dev = v3_dev;
	if ((e = tdfx_amix_attach(&c)) != 0)
		return e;
	v3_fronthook = tdfx_amix_front;
	return 0;
}

static int
tdfx_unload()
{
	int e;

	if ((e = tdfx_amix_detach()) != 0)
		return e;
	v3_fronthook = 0;
	return 0;
}

MOD_DRV_WRAPPER(tdfx, tdfx_load, tdfx_unload, 0, "3dfx DRM");
