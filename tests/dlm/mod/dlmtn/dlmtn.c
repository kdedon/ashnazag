/*
 * dlmtn -- an image larger than the loader takes fails before any
 * allocation.
 */

#include "sys/types.h"
#include "sys/errno.h"
#include "sys/moddefs.h"

static char big[3 * 1024 * 1024];

static int
dlmtn_load()
{
	return big[0] ? EINVAL : 0;
}

MOD_MISC_WRAPPER(dlmtn, dlmtn_load, 0, "image too large");
