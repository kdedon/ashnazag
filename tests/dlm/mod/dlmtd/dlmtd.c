/*
 * dlmtd -- depends on a module that does not exist: EINVAL.
 */

#include "sys/types.h"
#include "sys/moddefs.h"

MOD_MISC_WRAPPER(dlmtd, 0, 0, "missing dependency");
