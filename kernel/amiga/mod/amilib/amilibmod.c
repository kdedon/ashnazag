/*
 * The amilib module: an exec, expansion, utility, dos (read-only, one
 * directory), mmu (an adapter onto the kernel's MMU) and timer.device
 * for AmigaOS libraries run inside the kernel.  Modules that wrap such
 * a library ("$depend amilib") call am_init when they start one and
 * am_fini when it is gone; the environment exists while any of them
 * holds it.  See kernel/amiga/NOTES.md.
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
