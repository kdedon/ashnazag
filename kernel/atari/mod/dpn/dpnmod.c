/*
 * The BlueSCSI DaynaPORT Ethernet as a loadable STREAMS driver, major
 * 58 (/dev/dpn0): loaded on the first open, over the SCSI core.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/stream.h"
#include "sys/moddefs.h"

extern struct streamtab dpninfo;
extern int dp_unload();
static int dpn_flag;

struct mod_drv_data dpn_drvdata[] = {
	{ { 0 }, 0, 0,
	  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, &dpninfo, &dpn_flag }, 58, 1 }
};

static int
dpn_load()
{
	return 0;
}

MOD_DRV_WRAPPER(dpn, dpn_load, dp_unload, 0, "BlueSCSI DaynaPORT Ethernet");
