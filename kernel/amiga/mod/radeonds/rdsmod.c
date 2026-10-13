/*
 * The Radeon display as a loadable module: loading finds the card, sets
 * its mode and hands it to the display service; it refuses a machine
 * without one, or a cold card it cannot start from its ROM.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/moddefs.h"

extern int rds_attach(), rds_detach();

MOD_MISC_WRAPPER(radeonds, rds_attach, rds_detach, "Radeon display");
