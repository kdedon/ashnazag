/*
 * The Voodoo3 as a loadable module: loading finds the card, sets its
 * mode and hands it to the display service; it refuses a machine
 * without one.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/moddefs.h"

extern int v3ds_attach(), v3ds_detach();

MOD_MISC_WRAPPER(voodoo3, v3ds_attach, v3ds_detach, "Voodoo3 display");
