/*
 * Falcon SCSI: the 5380 behind the ST DMA chip.  Register r is selected
 * through the DMA mode register (0x88 + r), then read or written through
 * its data register.
 */
#include "scsi.h"

#ifdef FS_HOST
unsigned char	sim_rd();
void		sim_wr(), sim_us();
#define N_RD(r)		sim_rd(r)
#define N_WR(r, v)	sim_wr(r, v)
#define N_DELAY(n)	sim_us(n)
#else
#define DMADATA		(*(__volatile__ unsigned short *)0xFFFF8604)
#define DMAMODE		(*(__volatile__ unsigned short *)0xFFFF8606)
#define N_RD(r)		(DMAMODE = 0x88 + (r), (unsigned char)DMADATA)
#define N_WR(r, v)	(DMAMODE = 0x88 + (r), DMADATA = (v))
extern void delayus();
#define N_DELAY(n)	delayus(n)
#endif
#define N_PRESENT()	1
#define N_NAME		"Falcon SCSI"
#define N_HBA		scsi_falcon

#include "ncr5380.c"
