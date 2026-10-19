#include "sys/types.h"
#include "sys/param.h"
#include "sys/poll.h"
#include "sys/conf.h"
#include "sys/moddefs.h"
#include "sys/errno.h"
#include "ds.h"

extern int ds_fbopen(), ds_fbclose(), ds_fbread(), ds_fbioctl(), ds_fbmmap(), ds_fbpoll();
extern int ds_evopen(), ds_evclose(), ds_evread(), ds_evioctl(), ds_evpoll();
extern void dsuser_evpost(), dsuser_unloadsess();
extern int dsuser_segcount(), ds_segheld();
static int dsuser_flag;

struct mod_drv_data dsuser_drvdata[] = {
	{ { 0 }, 0, 0,
	  { ds_fbopen, ds_fbclose, ds_fbread, 0, ds_fbioctl, ds_fbmmap, ds_segmap, ds_fbpoll,
	    0, 0, 0, 0, &dsuser_flag }, 51, 1 },
	{ { 0 }, 0, 0,
	  { ds_evopen, ds_evclose, ds_evread, 0, ds_evioctl, 0, 0, ds_evpoll,
	    0, 0, 0, 0, &dsuser_flag }, 52, 2 },
	{ { 0 }, 0, 0, { 0 }, 0, 0 }
};

struct mod_hook_data dsuser_hookdata[] = {
	{ "ds_evpost", (int (*)())dsuser_evpost },
	{ "ds_unloadsess", (int (*)())dsuser_unloadsess },
	{ "ds_segcount", dsuser_segcount },
	{ 0, 0 }
};

static int
dsuser_unload()
{
	int i;
	struct dssess *s;

	if (ds_segheld())
		return EBUSY;
	for (i = 0; i < DS_NFBH; i++)
		if (ds_fbh[i].h_used)
			return EBUSY;
	for (i = 0; i < DS_NEVH; i++)
		if (ds_evh[i].e_used)
			return EBUSY;
	for (s = &ds_sess[1]; s <= &ds_sess[DS_NSESS]; s++)
		if (s->s_used)
			return EBUSY;
	return 0;
}

__asm__(".weak dsuser_conf_data");
extern struct mod_conf_data dsuser_conf_data;
static struct mod_type_data dsuser_fb_td = { "frame buffer", (char *)&dsuser_drvdata[0] };
static struct mod_type_data dsuser_ev_td = { "input devices", (char *)&dsuser_drvdata[1] };
static struct mod_type_data dsuser_hook_td = { "display callbacks", (char *)dsuser_hookdata };
static struct modlink dsuser_link[] = {
	{ &mod_drvops, (char *)&dsuser_fb_td },
	{ &mod_drvops, (char *)&dsuser_ev_td },
	{ &mod_hookops, (char *)&dsuser_hook_td },
	{ 0, 0 }
};
struct modwrapper dsuser_wrapper = {
	MODREV, 0, dsuser_unload, 0, &dsuser_conf_data, dsuser_link
};
