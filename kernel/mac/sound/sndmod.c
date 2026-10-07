/* /dev/snd as a loadable driver, major 47: loaded on the first open. */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/moddefs.h"

extern int sa_open(), sa_close(), sa_read(), sa_write(), sa_ioctl(), sa_poll();
static int auxsnd_flag;

struct mod_drv_data auxsnd_drvdata[] = {
	{ { 0 }, 0, 0,
	  { sa_open, sa_close, sa_read, sa_write, sa_ioctl, 0, 0, sa_poll,
	    0, 0, 0, 0, &auxsnd_flag }, 47, 1 }
};

static int
auxsnd_load()
{
	return 0;
}

static int
auxsnd_unload()
{
	return 0;
}

MOD_DRV_WRAPPER(auxsnd, auxsnd_load, auxsnd_unload, 0, "A/UX Sound Manager relay");
