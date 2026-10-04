/*
 * Console selection: cdevsw[0] (the console major) and oncons() both
 * name the streamtab "coinfo".  With the Amiga definition weakened,
 * this one makes /dev/console, /dev/syscon and /dev/systty (major 0,
 * minor 0) the SCC modem port.
 */
#include "sys/types.h"
#include "sys/stream.h"

extern struct qinit scc_rinit, scc_winit;

struct streamtab coinfo = {
	&scc_rinit, &scc_winit, 0, 0,
};
