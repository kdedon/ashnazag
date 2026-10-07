/* TT SCSI: the 5380's registers on odd bytes from 0xFFFF8781. */
#include "scsi.h"

#define TTREG(r)	(*(__volatile__ unsigned char *)(0xFFFF8781 + 2 * (r)))
#define N_RD(r)		TTREG(r)
#define N_WR(r, v)	(TTREG(r) = (v))
extern void delayus();
extern int ata_busprobe();
#define N_DELAY(n)	delayus(n)
#define N_PRESENT()	ata_busprobe(&TTREG(0))
#define N_NAME		"TT SCSI"
#define N_HBA		scsi_tt

#include "ncr5380.c"
