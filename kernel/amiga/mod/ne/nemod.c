/*
 * The PCI NE2000 as a loadable STREAMS driver, major 61 (/dev/ne0):
 * loaded on the first open.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/stream.h"
#include "sys/moddefs.h"

extern struct streamtab nuinfo;
extern int nu_unload();
static int ne_flag;

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
