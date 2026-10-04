/*
 * dlmtu -- calls a function the kernel does not have: ERELOC.
 */

#include "sys/types.h"
#include "sys/moddefs.h"

extern int dlm_no_such_symbol();

static int
dlmtu_load()
{
	return dlm_no_such_symbol();
}

MOD_MISC_WRAPPER(dlmtu, dlmtu_load, 0, "undefined symbol");
