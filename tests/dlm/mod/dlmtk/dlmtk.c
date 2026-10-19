/*
 * dlmtk -- a 4 MB sleeping kmem_alloc; dlmtk_ticks is how long it took.
 */

#include "sys/types.h"
#include "sys/errno.h"
#include "sys/kmem.h"
#include "sys/moddefs.h"

#define	SIZE	(4 * 1024 * 1024)

extern long lbolt;

static caddr_t dlmtk_p;
long	dlmtk_ticks;

static int
dlmtk_load()
{
	long t = lbolt;

	if ((dlmtk_p = kmem_alloc(SIZE, KM_SLEEP)) == 0)
		return ENOMEM;
	dlmtk_ticks = lbolt - t;
	return 0;
}

static int
dlmtk_unload()
{
	kmem_free(dlmtk_p, SIZE);
	return 0;
}

MOD_MISC_WRAPPER(dlmtk, dlmtk_load, dlmtk_unload, "waiting allocation");
