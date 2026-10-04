/*
 * SONIC chip core: rings, CAM, transmit and receive.  STREAMS-free, so
 * it also builds into the host test against a simulated chip (SN_HOST).
 */
#ifndef SONIC_H
#define SONIC_H

#include "snreg.h"

#define SN_NTX		8		/* transmit descriptors and buffers */
#define SN_NRX		32		/* receive descriptors and RBAs */
#define SN_BUFSZ	1536		/* per buffer; holds one 1518-byte frame */
#define SN_EOBCVAL	760		/* words: > one maximum frame, so one frame per RBA */
#define SN_MINFRAME	60		/* without CRC */
#define SN_MAXFRAME	1514		/* without CRC */
#define SN_CTLSZ	4096		/* descriptor area: one 4 KB block */
#define SN_POOLSZ	(SN_CTLSZ + (SN_NTX + SN_NRX) * SN_BUFSZ)
#define SN_POOLALLOC	(SN_POOLSZ + SN_CTLSZ)	/* slack for 4 KB alignment */

#define SN_NEXTTX(i)	(((i) + 1) & (SN_NTX - 1))
#define SN_NEXTRX(i)	(((i) + 1) & (SN_NRX - 1))
#define SN_PREVRX(i)	(((i) - 1) & (SN_NRX - 1))

#define SN_DCR_Q800	(DCR_EXBUS | DCR_BMS | DCR_DW | DCR_RFT1 | DCR_TFT0)
#define SN_IMRVAL	(INT_RFO | INT_RBAE | INT_RBE | INT_RDE | INT_TXER | INT_PTX | INT_PRX)

struct sn_stats {
	unsigned long	ipackets, ierrors, opackets, oerrors, collisions;
	unsigned long	crc, fae, rfo, rbae, rbe, rde, rxbad, br;
	unsigned long	txfu, txexc, txcarrier, txkick, reinit, nomem;
	unsigned long	intr, camfail;
};

struct sn_softc {
	/* descriptor area and buffers (addresses are the chip's) */
	struct sn_tda	*tda;
	struct sn_rda	*rda;
	struct sn_rra	*rra;
	struct sn_cda	*cda;		/* SN_NCAM + 1 (the enable word) */
	unsigned char	*txbuf, *rxbuf;

	int		txhead;		/* next descriptor to fill */
	int		txtail;		/* oldest one the chip owns */
	int		txcnt;
	int		rxnext;		/* next receive descriptor to look at */
	int		txidle;		/* watchdog ticks without tx progress */

	unsigned short	rcr;		/* RCR_BRD, RCR_PRO, RCR_AMC */
	unsigned short	camvalid;	/* bit n: CAM entry n in use */
	unsigned char	cam[SN_NCAM][6];	/* entry 0: station address */
	int		running;
	struct sn_stats	st;
};

/* core entry points */
int	sn_attach();		/* (sc, pool) carve the pool */
int	sn_init();		/* (sc) reset and start; 0 or -1 */
void	sn_stop();		/* (sc) */
int	sn_camload();		/* (sc) 0 or -1; receiver off */
int	sn_filter();		/* (sc) reload CAM and RCR while running */
int	sn_txroom();		/* (sc) */
unsigned char *sn_txbuf();	/* (sc) buffer for the next frame */
void	sn_txstart();		/* (sc, len) hand the frame to the chip */
int	sn_intr();		/* (sc) service; 1 if anything was pending */
int	sn_watch();		/* (sc) once a second */

/* supplied by the caller */
void	sn_input();		/* (sc, pkt, len without CRC, status) */
void	sn_txdone();		/* (sc) descriptors were freed */

/*
 * Platform: register access, chip addresses, cache maintenance.
 * The kernel keeps the pool in .bss, reached through DTT0 (VA = PA).
 * The descriptor block must be cache-inhibited or write-through, never
 * copyback: the CPU rewrites an RDA link in the line where the chip
 * writes that descriptor's in_use, and a line push would undo the chip.
 */
#ifdef SN_HOST
unsigned short	sim_rd();
void		sim_wr();
sn_u32		sim_pa();
unsigned char	*sim_va();
#define SN_RD(r)	sim_rd(r)
#define SN_WR(r, v)	sim_wr((r), (v))
#define SN_PA(p)	sim_pa((unsigned char *)(p))
#define SN_VA(a)	sim_va((sn_u32)(a))
#define SN_PUSH(p, n)
#define SN_INVAL(p, n)
#define SN_SYNC()
#define SN_DELAY(n)
#else
#define VOL		__volatile__
#define SN_REGBASE	0x50F0A000
#define SN_REG(r)	(*(VOL unsigned short *)(SN_REGBASE + (r) * 4 + 2))
#define SN_RD(r)	SN_REG(r)
#define SN_WR(r, v)	(SN_REG(r) = (v))
#define SN_PA(p)	((sn_u32)(p))
#define SN_VA(a)	((unsigned char *)(a))
extern void sn_dcpush(), sn_dcinval();
#define SN_PUSH(p, n)	sn_dcpush((char *)(p), (long)(n))
#define SN_INVAL(p, n)	sn_dcinval((char *)(p), (long)(n))
/* drains the 68040 store buffer before the chip is told to look */
#define SN_SYNC()	__asm__ __volatile__("nop" : : : "memory")
extern int delayus();
#define SN_DELAY(n)	delayus(n)
#endif

#endif
