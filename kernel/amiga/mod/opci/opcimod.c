/*
 * The opci module: the PCI bus behind a Mediator, Prometheus, G-REX or
 * Firestorm bridge, for other modules (<sys/opci.h>, "$depend opci").
 *
 * Loading reads openpci.library (Thomas Richter's, from Aminet; not
 * shipped here) from opci_confdir and starts it on amilib; the
 * library reads its PCI-Configuration, and loads any plugins, from the
 * same directory.  A kernel
 * without a bridge, or without the Amiga platform hooks, refuses the
 * load with ENXIO.  Unloading expunges the library; while a dependent
 * module is loaded DLM keeps opci.
 */
#include "sys/types.h"
#include "sys/moddefs.h"

extern char opci_confdir[];
extern int opci_init(), opci_fini();

static int
opci_load()
{
	return opci_init(opci_confdir);
}

static int
opci_unload()
{
	return opci_fini();
}

MOD_MISC_WRAPPER(opci, opci_load, opci_unload, "PCI bridges (openpci.library)");
