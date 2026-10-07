/*
 * /dev/asc as a loadable driver, major 46: loaded on the first open.
 * The VIA2 CB1 interrupt reaches snd_intr through snd_intrfn while
 * the module is in.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/moddefs.h"

extern int snd_open(), snd_close(), snd_read(), snd_mmap(), snd_poll(), spec_segmap();
extern void snd_intr();
extern void (*volatile snd_intrfn)();
static int asc_flag;

struct mod_drv_data asc_drvdata[] = {
	{ { 0 }, 0, 0,
	  { snd_open, snd_close, snd_read, 0, 0, snd_mmap, spec_segmap, snd_poll,
	    0, 0, 0, 0, &asc_flag }, 46, 1 }
};

static int
asc_load()
{
	snd_intrfn = snd_intr;
	return 0;
}

/* only when closed: the chip is quiet, its interrupt off */
static int
asc_unload()
{
	snd_intrfn = 0;
	return 0;
}

MOD_DRV_WRAPPER(asc, asc_load, asc_unload, 0, "Apple Sound Chip");
