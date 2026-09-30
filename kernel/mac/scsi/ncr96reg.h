/*
 * Quadra 800 on-board NCR 53C96: registers and board glue.
 *
 * Chip register n is at NCR_BASE + n*16.  NCR_PDMA is the pseudo-DMA
 * port: each 16-bit access there moves two bytes through the chip FIFO
 * with a DACK handshake; the bus cycle stalls until the chip is ready.
 * V2_DREQ reads the chip's DREQ line (bit 0 of a long read).
 *
 * All chip, port and VIA accesses go through the macros below; with
 * NCR_HOST they call the host test's chip simulator.
 */

#define NCR_BASE	0x50F10000
#define NCR_PDMA	(NCR_BASE + 0x100)
#define VIA2_BASE	0x50F02000
#define V2_DREQ		0x50F03A00

#define NCR_ADDR(r)	(NCR_BASE + ((r) << 4))

#ifdef NCR_HOST
extern int	sim_rd(), sim_prd(), sim_via_rd();
extern void	sim_wr(), sim_pwr(), sim_via_wr();
#define NCR_RD(r)	((unsigned char)sim_rd(r))
#define NCR_WR(r, v)	sim_wr(r, v)
#define PDMA_PORT	((unsigned short *)0)
#define PDMA_RD(p)	((unsigned short)sim_prd())
#define PDMA_WR(p, w)	sim_pwr(w)
#define VIA2_RD(r)	((unsigned char)sim_via_rd(r))
#define VIA2_WR(r, v)	sim_via_wr(r, v)
#else
#define NCR_RD(r)	(*(__volatile__ unsigned char *)NCR_ADDR(r))
#define NCR_WR(r, v)	(*(__volatile__ unsigned char *)NCR_ADDR(r) = (v))
#define PDMA_PORT	((__volatile__ unsigned short *)NCR_PDMA)
#define PDMA_RD(p)	(*(p))
#define PDMA_WR(p, w)	(*(p) = (w))
#define VIA2_RD(r)	(*(__volatile__ unsigned char *)(VIA2_BASE + (r)))
#define VIA2_WR(r, v)	(*(__volatile__ unsigned char *)(VIA2_BASE + (r)) = (v))
#endif

/* chip registers */
#define TCL		0x0	/* transfer count low */
#define TCM		0x1	/* transfer count high (16-bit counter) */
#define FIFO		0x2
#define CMD		0x3
#define STAT		0x4	/* read */
#define SELID		0x4	/* write: destination bus ID */
#define INTR		0x5	/* read; reading clears the interrupt */
#define SELTO		0x5	/* write: select timeout */
#define STEP		0x6	/* read: sequence step */
#define SYNCTP		0x6	/* write: sync period */
#define FFLAG		0x7	/* read: FIFO byte count in bits 0-4 */
#define SYNCOFF		0x7	/* write: sync offset, 0 = async */
#define CFG1		0x8
#define CCF		0x9	/* clock conversion factor */
#define CFG2		0xB
#define CFG3		0xC

/* commands */
#define C_DMA		0x80
#define C_NOP		0x00
#define C_FLUSH		0x01
#define C_RSTCHIP	0x02
#define C_RSTSCSI	0x03
#define C_TRANS		0x10	/* transfer information */
#define C_ICCS		0x11	/* initiator command complete: status + message */
#define C_MSGOK		0x12	/* message accepted: drop ACK */
#define C_SETATN	0x1A
#define C_SELATN	0x42	/* select, ATN, one message byte, CDB */
#define C_ENSEL		0x44	/* enable selection/reselection */

/* STAT */
#define S_INT		0x80
#define S_GE		0x40
#define S_PE		0x20
#define S_TC		0x10	/* transfer count zero */
#define S_PHASE		0x07

/* INTR */
#define I_SBR		0x80	/* SCSI bus reset */
#define I_ILL		0x40	/* illegal command */
#define I_DIS		0x20	/* disconnect / select timeout */
#define I_BS		0x10	/* bus service */
#define I_FC		0x08	/* function complete */
#define I_RESEL		0x04	/* reselected */

#define FF_COUNT	0x1F
#define FIFO_SIZE	16

/* SCSI phases (STAT & S_PHASE) */
#define P_DATAOUT	0
#define P_DATAIN	1
#define P_CMD		2
#define P_STATUS	3
#define P_MSGOUT	6
#define P_MSGIN		7

/* values used by A/UX for a 25 MHz chip clock */
#define NCR_CCF		5
#define NCR_SELTO	0xA4
#define NCR_CFG1	0x47	/* bus ID 7, reset interrupt disabled */
#define NCR_CFG2	0x00
#define NCR_CFG3	0x04	/* 10-byte group 2 CDBs */
#define NCR_ID		7

/* VIA2: CB2 = SCSI IRQ (IFR bit 3) */
#define VIA_PCR		0x1800
#define VIA_IFR		0x1A00
#define VIA_IER		0x1C00
#define V2_SCSIIRQ	0x08

/* SCSI messages */
#define M_CMDCOMPLETE	0x00
#define M_EXTENDED	0x01
#define M_SAVEDP	0x02
#define M_RESTOREDP	0x03
#define M_DISCONNECT	0x04
#define M_ABORT		0x06
#define M_REJECT	0x07
#define M_NOP		0x08
#define M_IDENTIFY	0x80	/* LUN 0 */
#define M_DISCPRIV	0x40	/* IDENTIFY: may disconnect */
