/*
 * The ZZ9000 as a loadable driver: /dev/zz9000 (major 49) for the RTG
 * X servers, and the display service's screen.  Loading refuses a
 * machine without the card.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/errno.h"
#include "sys/moddefs.h"

extern int zz9000open(), zz9000close(), zz9000read(), zz9000write();
extern int zz9000ioctl(), zz9000mmap(), spec_segmap();
extern void zz9000init();
extern int zzds_attach(), zzds_detach();
static int zz9000_flag;

struct mod_drv_data zz9000_drvdata[] = {
	{ { 0 }, 0, 0,
	  { zz9000open, zz9000close, zz9000read, zz9000write, zz9000ioctl,
	    zz9000mmap, spec_segmap, 0, 0, 0, 0, 0, &zz9000_flag }, 49, 1 }
};

static int
zz9000_load()
{
	zz9000init();
	return zzds_attach();
}

static int
zz9000_unload()
{
	return zzds_detach();
}

MOD_DRV_WRAPPER(zz9000, zz9000_load, zz9000_unload, 0, "MNT ZZ9000");
