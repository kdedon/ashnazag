/*
 * The amilib module: a shim answering the calls an AmigaOS library
 * makes into exec, expansion, utility, dos (read-only, one directory),
 * mmu (an adapter onto the kernel's MMU) and timer.device, so the
 * kernel can load the library and call it.  No AmigaOS runs.  Modules
 * that wrap such a library ("$depend amilib") call am_init when they
 * start one and am_fini when it is gone; the shim's data exists while
 * any of them holds it.
 */
#include "sys/types.h"
#include "sys/errno.h"
#include "sys/moddefs.h"
#include "amilib.h"

static int
amilib_load()
{
	return 0;
}

/* DLM keeps us while a dependent is loaded; this is the second guard */
static int
amilib_unload()
{
	return am_users ? EBUSY : 0;
}

MOD_MISC_WRAPPER(amilib, amilib_load, amilib_unload, "AmigaOS libraries");
