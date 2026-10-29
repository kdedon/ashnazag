/*
 * The MiSTer RTG board as a loadable module: loading finds the board,
 * sets its mode and hands it to the display service; it refuses a
 * machine without one.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/moddefs.h"

extern int mr_attach(), mr_detach();

MOD_MISC_WRAPPER(mistrtg, mr_attach, mr_detach, "MiSTer RTG display");
