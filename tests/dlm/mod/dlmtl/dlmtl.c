/*
 * dlmtl -- _load fails with ENODEV, which modload passes through.
 */

#include "sys/types.h"
#include "sys/errno.h"
#include "sys/moddefs.h"

static int
dlmtl_load()
{
	return ENODEV;
}

MOD_MISC_WRAPPER(dlmtl, dlmtl_load, 0, "failing load");
