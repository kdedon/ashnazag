/*
 * The PCI NE2000 or, built with NU_XSURF, the X-Surf 100 as a loadable
 * STREAMS driver, major 61 (/dev/ne0): loaded on the first open.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/stream.h"
#include "sys/moddefs.h"

extern struct streamtab nuinfo;
extern int nu_unload();
static int ne_flag;

#ifdef NU_XSURF
#include "sys/errno.h"

extern int autocon();

struct mod_drv_data xsurf_drvdata[] = {
	{ { 0 }, 0, 0,
	  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, &nuinfo, &ne_flag }, 61, 1 }
};

/* no board, no module: the PCI one may take major 61 instead */
static int
xsurf_load()
{
	unsigned long base, size;

	return autocon(0x12120064, 0, &base, &size) ? 0 : ENXIO;
}

MOD_DRV_WRAPPER(xsurf, xsurf_load, nu_unload, 0, "X-Surf 100 Ethernet");
#else
struct mod_drv_data ne_drvdata[] = {
	{ { 0 }, 0, 0,
	  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, &nuinfo, &ne_flag }, 61, 1 }
};

static int
ne_load()
{
	return 0;
}

MOD_DRV_WRAPPER(ne, ne_load, nu_unload, 0, "PCI NE2000 Ethernet");
#endif
