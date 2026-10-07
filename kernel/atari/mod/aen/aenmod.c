/*
 * The NetUSBee Ethernet as a loadable STREAMS driver, major 18
 * (/dev/aen0): loaded on the first open.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/stream.h"
#include "sys/moddefs.h"

extern struct streamtab nuinfo;
extern int nu_unload();
static int aen_flag;

struct mod_drv_data aen_drvdata[] = {
	{ { 0 }, 0, 0,
	  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, &nuinfo, &aen_flag }, 18, 1 }
};

static int
aen_load()
{
	return 0;
}

MOD_DRV_WRAPPER(aen, aen_load, nu_unload, 0, "NetUSBee Ethernet");
