/* The SCSI core and host adapters, for the drivers that use the bus. */
#include "sys/types.h"
#include "sys/moddefs.h"

static int
scsi_load()
{
	return 0;
}

static int
scsi_unload()
{
	return 0;
}

MOD_MISC_WRAPPER(scsi, scsi_load, scsi_unload, "SCSI core");
