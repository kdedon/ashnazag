/*
 * NCR 5380 registers.  An adapter file defines N_RD(r), N_WR(r, v),
 * N_DELAY(us), N_PRESENT(), N_NAME and N_HBA, then includes ncr5380.c.
 * Polled PIO only; no interrupts are enabled.
 */
#ifndef NCR5380_H
#define NCR5380_H

#include "scsi.h"

/* 5380 registers: read / write */
#define NCR_DATA	0	/* current data / output data */
#define NCR_ICR		1	/* initiator command */
#define NCR_MR		2	/* mode */
#define NCR_TCR		3	/* target command */
#define NCR_CSBR	4	/* bus status / select enable */
#define NCR_BSR		5	/* bus and status / start DMA send */
#define NCR_IDR		6	/* input data / start DMA target receive */
#define NCR_RPI		7	/* reset parity and interrupt */

#define ICR_RST		0x80
#define ICR_AIP		0x40	/* read: arbitration in progress */
#define ICR_LA		0x20	/* read: lost arbitration */
#define ICR_ACK		0x10
#define ICR_BSY		0x08
#define ICR_SEL		0x04
#define ICR_ATN		0x02
#define ICR_DATA	0x01

#define MR_ARB		0x01

#define CSB_RST		0x80
#define CSB_BSY		0x40
#define CSB_REQ		0x20
#define CSB_MSG		0x10
#define CSB_CD		0x08
#define CSB_IO		0x04
#define CSB_SEL		0x02
#define CSB_PHASE	(CSB_MSG | CSB_CD | CSB_IO)

#define PH_DOUT		0
#define PH_DIN		CSB_IO
#define PH_CMD		CSB_CD
#define PH_STAT		(CSB_CD | CSB_IO)
#define PH_MOUT		(CSB_MSG | CSB_CD)
#define PH_MIN		(CSB_MSG | CSB_CD | CSB_IO)

#endif
