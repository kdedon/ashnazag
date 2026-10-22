/*
 * The 53C96 as a boot module: attaches as card 0 and takes the VIA2
 * CB2 interrupt through scsi_intrfn.  It serves the disks, so it never
 * unloads.
 */
#include "sys/types.h"
#include "sys/errno.h"
#include "sys/moddefs.h"
#include "rico.h"
#include "sd.h"
#include "ncr96reg.h"

extern void ncr96init(), ncr96queue(), ncr96intr(), dlm_cachepush();
extern int sd_attach();
extern void sdc_attach();
extern void (*__volatile__ scsi_intrfn)();

static int
ncr96open(c)
uint c;
{
	ncr96init(1);
	return 0;
}

static bool
ncr96q(c, cp)
uint c;
struct sdcom *cp;
{
	/* the transfer runs through the physical window */
	if (!(cp->reading & 2))
		dlm_cachepush(cp->addr, cp->nbyte);
	ncr96queue(cp);
	return TRUE;
}

#ifndef BOOTDIAG
static
#endif
int
ncr96_load()
{
	int e;

	if ((e = sd_attach(ncr96open, ncr96q, 0, (char *)NCR_BASE,
	    "Quadra 53C96 SCSI")) == 0) {
		scsi_intrfn = ncr96intr;
		sdc_attach();
	}
	return e;
}

/* a BOOTDIAG kernel links it in: the status lines read its counters */
#ifndef BOOTDIAG
static int
ncr96_unload()
{
	return EBUSY;
}

MOD_MISC_WRAPPER(ncr96, ncr96_load, ncr96_unload, "Quadra 53C96 SCSI");
#endif
