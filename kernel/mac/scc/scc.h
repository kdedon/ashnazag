/*
 * Quadra 800 Z85C30 SCC: register bits and per-channel state.
 *
 * Channel A (modem port) control at base+2, data at base+6;
 * channel B (printer port) control at base+0, data at base+4.
 */

#define VOL	__volatile__

/* WR0 commands */
#define W0_RSTEXT	0x10	/* reset external/status interrupts */
#define W0_RSTTXIP	0x28	/* reset Tx interrupt pending */
#define W0_RSTERR	0x30	/* error reset */
#define W0_RSTIUS	0x38	/* reset highest IUS */

/* WR1 */
#define W1_RIE		0x10	/* Rx interrupt on all characters */
#define W1_TIE		0x02
#define W1_SIE		0x01

/* WR3 */
#define W3_RX5		0x00
#define W3_RX7		0x40
#define W3_RX6		0x80
#define W3_RX8		0xC0
#define W3_RXEN		0x01

/* WR4 */
#define W4_X16		0x40
#define W4_1SB		0x04
#define W4_2SB		0x0C
#define W4_EVEN		0x02
#define W4_PAREN	0x01

/* WR5 */
#define W5_DTR		0x80	/* HSKo on the Mac connector */
#define W5_TX5		0x00
#define W5_TX7		0x20
#define W5_TX6		0x40
#define W5_TX8		0x60
#define W5_BREAK	0x10
#define W5_TXEN		0x08
#define W5_RTS		0x02

/* WR9 */
#define W9_MIE		0x08
#define W9_NV		0x02

/* WR11: Rx and Tx clocks from the BRG */
#define W11_BRG		0x50

/* WR14: BRG on, clocked from RTxC (3.6864 MHz) */
#define W14_BRGEN	0x01

/* WR15 */
#define W15_BRKIE	0x80
#define W15_CTSIE	0x20
#define W15_DCDIE	0x08

/* RR0 */
#define R0_BREAK	0x80
#define R0_CTS		0x20
#define R0_DCD		0x08
#define R0_TXRDY	0x04
#define R0_RXRDY	0x01

/* RR1 */
#define R1_FE		0x40
#define R1_OE		0x20
#define R1_PE		0x10
#define R1_ALLSENT	0x01

/* RR3 (channel A only) */
#define R3_ARX		0x20
#define R3_ATX		0x10
#define R3_AEXT		0x08
#define R3_BRX		0x04
#define R3_BTX		0x02
#define R3_BEXT		0x01
#define R3_A		(R3_ARX|R3_ATX|R3_AEXT)
#define R3_B		(R3_BRX|R3_BTX|R3_BEXT)

/*
 * HSKi reaches the CTS input inverted: the RR0 CTS bit is clear while
 * the peer asserts its handshake line.
 */
#define CTS_ON(rr0)	(((rr0) & R0_CTS) == 0)

/* Minor number: bit 0 channel (0 = A, modem port; 1 = B, printer port) */
#define SCC_CHAN(m)	((m) & 1)
#define SCC_MODEM	0x80	/* DCD gates open, carrier loss hangs up */
#define SCC_HWFLOW	0x40	/* CTS (HSKi) stops output, DTR throttles input */
#define SCC_MINMASK	(SCC_MODEM|SCC_HWFLOW|1)

#define SCC_NCHAN	2

/* ring sizes, powers of two */
#define RBSZ		512
#define TBSZ		256
#define RHIWAT		(RBSZ - 64)
#define RLOWAT		(RBSZ / 4)
#define TLOWAT		(TBSZ / 4)

/* receive ring entry: character in the low byte, status above */
#define RX_BRK		0x100
#define RX_PE		0x200
#define RX_FE		0x400
#define RX_OE		0x800

/* sc_flags */
#define SCF_HWINIT	0x0001	/* channel registers loaded */
#define SCF_TXBUSY	0x0002	/* a character is in the transmitter */
#define SCF_DRAIN	0x0004	/* wsrv waits for the transmitter */
#define SCF_BREAK	0x0008	/* sending break */
#define SCF_BRKEND	0x0010	/* discard the null after a received break */
#define SCF_RBLOCK	0x0020	/* input throttled by the ring */
#define SCF_IBLOCK	0x0040	/* input throttled by M_STOPI */
#define SCF_HUP		0x0080	/* carrier lost; rsrv sends M_HANGUP */

struct scc {
	VOL unsigned char *sc_ctl;
	VOL unsigned char *sc_data;
	int		sc_minor;
	queue_t		*sc_rq;
	VOL int		sc_flags;
	int		sc_tid;		/* M_DELAY / break timeout */
	int		sc_did;		/* drain poll timeout */
	int		sc_bid;		/* bufcall */
	int		sc_mctl;	/* TIOCM_DTR/TIOCM_RTS as last set */
	struct strtty	sc_tty;
	unsigned char	sc_wr[16];	/* soft copy of the write registers */
	VOL unsigned char sc_rr0;
	VOL unsigned char sc_xchar;	/* flow-control character to send */
	VOL int		sc_rput, sc_rget;
	VOL int		sc_tput, sc_tget;
	unsigned short	sc_rbuf[RBSZ];
	unsigned char	sc_tbuf[TBSZ];
	unsigned long	sc_nrx, sc_ntx, sc_novf, sc_noe, sc_npe, sc_nfe, sc_nbrk;
};

#define RCOUNT(sp)	(((sp)->sc_rput - (sp)->sc_rget) & (RBSZ - 1))
#define TCOUNT(sp)	(((sp)->sc_tput - (sp)->sc_tget) & (TBSZ - 1))
