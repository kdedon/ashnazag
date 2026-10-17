/*
 * ti.h -- the Voyage 200 profile: one calculator at a time.
 */

#include "sys/types.h"
#include "sys/conf.h"
#include "kinc.h"
#include "sys/sysm68k.h"
#include "profile.h"

#define	SR_S	0x2000
#define	SR_IPL	0x0700
#define	TI_SIG	17		/* SIGUSR2: carries the calculator's interrupts */

