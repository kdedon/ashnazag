/*
 * National DP83932 SONIC: register numbers, bits and the 32-bit-mode
 * memory structures.  Names follow the datasheet.  Every descriptor
 * field is a longword whose value is in the low 16 bits.
 */
#ifndef SNREG_H
#define SNREG_H

/* register numbers (register n is at base + 4n + 2 on the Quadra) */
#define SN_CR		0x00	/* command */
#define SN_DCR		0x01	/* data configuration */
#define SN_RCR		0x02	/* receive control */
#define SN_TCR		0x03	/* transmit control */
#define SN_IMR		0x04	/* interrupt mask */
#define SN_ISR		0x05	/* interrupt status (write 1 to clear) */
#define SN_UTDA		0x06	/* upper transmit descriptor address */
#define SN_CTDA		0x07	/* current transmit descriptor address */
#define SN_URDA		0x0d	/* upper receive descriptor address */
#define SN_CRDA		0x0e	/* current receive descriptor address */
#define SN_EOBC		0x13	/* end of buffer word count */
#define SN_URRA		0x14	/* upper receive resource address */
#define SN_RSA		0x15	/* resource start address */
#define SN_REA		0x16	/* resource end address */
#define SN_RRP		0x17	/* resource read pointer */
#define SN_RWP		0x18	/* resource write pointer */
#define SN_CEP		0x21	/* CAM entry pointer */
#define SN_CAP2		0x22	/* CAM address port 2 */
#define SN_CAP1		0x23	/* CAM address port 1 */
#define SN_CAP0		0x24	/* CAM address port 0 */
#define SN_CE		0x25	/* CAM enable */
#define SN_CDP		0x26	/* CAM descriptor pointer */
#define SN_CDC		0x27	/* CAM descriptor count */
#define SN_SR		0x28	/* silicon revision */
#define SN_WT0		0x29	/* watchdog timer 0 */
#define SN_WT1		0x2a	/* watchdog timer 1 */
#define SN_RSC		0x2b	/* receive sequence counter */
#define SN_CRCT		0x2c	/* CRC error tally */
#define SN_FAET		0x2d	/* frame alignment error tally */
#define SN_MPT		0x2e	/* missed packet tally */
#define SN_DCR2		0x3f	/* data configuration 2 */
#define SN_NREGS	0x40

/* CR */
#define CR_HTX		0x0001	/* halt transmission */
#define CR_TXP		0x0002	/* transmit packets */
#define CR_RXDIS	0x0004	/* receiver disable */
#define CR_RXEN		0x0008	/* receiver enable */
#define CR_STP		0x0010	/* stop timer */
#define CR_ST		0x0020	/* start timer */
#define CR_RST		0x0080	/* software reset */
#define CR_RRRA		0x0100	/* read RRA */
#define CR_LCAM		0x0200	/* load CAM */

/* DCR */
#define DCR_TFT0	0x0001	/* transmit FIFO threshold */
#define DCR_TFT1	0x0002
#define DCR_RFT0	0x0004	/* receive FIFO threshold */
#define DCR_RFT1	0x0008
#define DCR_BMS		0x0010	/* block mode DMA */
#define DCR_DW		0x0020	/* 32-bit data width */
#define DCR_EXBUS	0x8000	/* extended bus mode */

/* RCR (also the receive descriptor status) */
#define RCR_PRX		0x0001	/* packet received OK */
#define RCR_LBK		0x0002
#define RCR_FAER	0x0004	/* frame alignment error */
#define RCR_CRCR	0x0008	/* CRC error */
#define RCR_LPKT	0x0040	/* last packet in RBA */
#define RCR_BC		0x0080	/* broadcast */
#define RCR_MC		0x0100	/* multicast */
#define RCR_AMC		0x0800	/* accept all multicast */
#define RCR_PRO		0x1000	/* physical promiscuous */
#define RCR_BRD		0x2000	/* accept broadcast */
#define RCR_RNT		0x4000	/* accept runts */
#define RCR_ERR		0x8000	/* accept errored packets */

/* TCR (also the transmit descriptor status) */
#define TCR_PTX		0x0001	/* packet transmitted OK */
#define TCR_BCM		0x0002	/* byte count mismatch */
#define TCR_FU		0x0004	/* FIFO underrun */
#define TCR_PMB		0x0008
#define TCR_OWC		0x0020	/* out of window collision */
#define TCR_EXC		0x0040	/* excessive collisions */
#define TCR_CRSL	0x0080	/* carrier sense lost */
#define TCR_NCRS	0x0100	/* no carrier sense */
#define TCR_DEF		0x0200	/* deferred */
#define TCR_EXD		0x0400	/* excessive deferral */
#define TCR_NCOL(s)	(((s) >> 11) & 0x1f)
#define TCR_CFG		0xf000	/* configuration bits in the status word */

/* IMR / ISR */
#define INT_RFO		0x0001	/* receive FIFO overrun */
#define INT_MP		0x0002	/* missed packet tally rollover */
#define INT_FAE		0x0004
#define INT_CRC		0x0008
#define INT_RBAE	0x0010	/* packet larger than the RBA */
#define INT_RBE		0x0020	/* receive buffers exhausted */
#define INT_RDE		0x0040	/* receive descriptors exhausted */
#define INT_TC		0x0080
#define INT_TXER	0x0100	/* transmit error */
#define INT_PTX		0x0200	/* packet transmitted */
#define INT_PRX		0x0400	/* packet received */
#define INT_PINT	0x0800
#define INT_LCD		0x1000	/* CAM load done */
#define INT_HBL		0x2000
#define INT_BR		0x4000	/* bus retry */
#define INT_ALL		0x7fff

/* link fields */
#define LINK_EOL	0x0001

#ifdef SN_HOST
typedef unsigned int sn_u32;
#else
typedef unsigned long sn_u32;
#endif

/* receive resource (one RBA) */
struct sn_rra {
	sn_u32	ptr0, ptr1;		/* buffer address, low / high */
	sn_u32	wc0, wc1;		/* size in 16-bit words, low / high */
};

/* receive descriptor */
struct sn_rda {
	sn_u32	status;			/* RCR bits */
	sn_u32	count;			/* bytes, including the CRC */
	sn_u32	ptr0, ptr1;		/* packet address in the RBA */
	sn_u32	seq;			/* RBA sequence << 8 | packet sequence */
	sn_u32	link;			/* next descriptor (low 16) | EOL */
	sn_u32	inuse;			/* 0 once the SONIC has filled it */
};

/* transmit descriptor with one fragment, then the link */
struct sn_tda {
	sn_u32	status;			/* TCR bits */
	sn_u32	config;			/* TCR configuration bits */
	sn_u32	size;			/* packet size */
	sn_u32	nfrag;			/* fragment count */
	sn_u32	fptr0, fptr1;		/* fragment address */
	sn_u32	fsize;			/* fragment size */
	sn_u32	link;			/* next descriptor (low 16) | EOL */
};

/* CAM descriptor */
struct sn_cda {
	sn_u32	entry;			/* CAM entry number */
	sn_u32	port0, port1, port2;	/* address, 2 bytes each, low byte first */
};

#define SN_NCAM		16

#endif
