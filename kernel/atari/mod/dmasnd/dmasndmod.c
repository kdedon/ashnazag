/* /dev/dmasnd as a loadable driver, major 46: loaded on the first open. */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/moddefs.h"

extern int dma_open(), dma_close(), dma_read(), dma_write(), dma_ioctl(), dma_poll();
static int dmasnd_flag;

struct mod_drv_data dmasnd_drvdata[] = {
	{ { 0 }, 0, 0,
	  { dma_open, dma_close, dma_read, dma_write, dma_ioctl, 0, 0, dma_poll,
	    0, 0, 0, 0, &dmasnd_flag }, 46, 1 }
};

static int
dmasnd_load()
{
	return 0;
}

/* only when closed: the hooks are clear */
static int
dmasnd_unload()
{
	return 0;
}

MOD_DRV_WRAPPER(dmasnd, dmasnd_load, dmasnd_unload, 0, "Atari DMA sound");
