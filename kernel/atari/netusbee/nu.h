/*
 * NetUSBee Ethernet chip core: probe, ring, transmit, receive.  No
 * interrupt line reaches the cartridge port, so the caller polls nu_poll.
 * STREAMS-free; builds on the host against a simulated card (NU_HOST).
 */
#ifndef NU_H
#define NU_H

#include "nureg.h"

#define NU_NTX		2		/* transmit buffers, 6 pages each */
#define NU_TXPAGES	6
#define NU_MINFRAME	60		/* without CRC */
#define NU_MAXFRAME	1514		/* without CRC */
#define NU_RXBURST	8		/* frames per poll */
#define NU_NMC		16		/* multicast addresses */

struct nu_stats {
	unsigned long	ipackets, ierrors, opackets, oerrors, collisions;
	unsigned long	crc, fae, missed, ovw, rxbad;
	unsigned long	txfu, txabt, txcarrier, txowc, txtimeout, reinit, nomem;
	unsigned long	polls;
};

struct nu_softc {
	int		memlo, memhi;	/* buffer memory pages [lo, hi) */
	int		rxstart, rxstop;
	int		rxnext;		/* next page to read */
	int		txlen[NU_NTX];	/* 0 free, else queued length */
	int		txcur;		/* buffer on the wire, -1 none */
	int		txfill;		/* next buffer to fill */
	int		txage;		/* polls the current frame has taken */
	int		rtl;		/* RTL8019 ID seen */
	unsigned char	ea[6];
	unsigned char	prom[6];
	unsigned char	rcr;		/* RCR_AB, RCR_AM, RCR_PRO */
	unsigned char	mar[8];
	int		nmc;
	unsigned char	mc[NU_NMC][6];
	int		mcref[NU_NMC];
	int		running;
	unsigned char	txbuf[NU_MAXFRAME];
	unsigned char	rxbuf[NU_MAXFRAME + 4];
	struct nu_stats	st;
};

int	nu_probe();		/* (sc) 0 found, -1 none */
int	nu_init();		/* (sc) 0 or -1 */
void	nu_stop();		/* (sc) */
int	nu_txroom();		/* (sc) */
void	nu_txstart();		/* (sc, len) send sc->txbuf */
int	nu_poll();		/* (sc) once a tick; 1 if any work was done */
int	nu_filter();		/* (sc) load station address, RCR and hash */
int	nu_addmc();		/* (sc, addr) 0, or -1 when full */
int	nu_delmc();		/* (sc, addr) 0, or -1 when absent */

/* supplied by the caller */
void	nu_input();		/* (sc, frame, len without CRC) */
void	nu_txdone();		/* (sc) a transmit buffer became free */

#ifdef NU_HOST
unsigned char	sim_bus();
#define NU_RD(r)	sim_bus((unsigned long)NU_RDBASE + ((r) << NU_REGSHIFT))
#define NU_WR(r, v)	((void)sim_bus((unsigned long)NU_WRBASE + \
			    ((r) << NU_REGSHIFT) + (((v) & 0xff) << 1)))
#define NU_DELAY(n)
#define NU_YIELD()
extern int sim_clkdue;
#define NU_CLKDUE()	sim_clkdue
#else
#define VOL		__volatile__
#define NU_RD(r)	(*(VOL unsigned char *)(NU_RDBASE + ((r) << NU_REGSHIFT)))
/* the read is the write: it must reach the bus */
#define NU_WR(r, v)	__asm__ __volatile__("tstb %0@" : : "a" \
			    (NU_WRBASE + ((r) << NU_REGSHIFT) + (((v) & 0xff) << 1)) \
			    : "memory")
extern int delayus();
#define NU_DELAY(n)	delayus(n)
/*
 * Copies run at IPL 6 and take up to 2 ms a frame; the keyboard ACIA
 * holds one byte (1.3 ms at 7812 baud), so drain it between chunks.
 */
extern void ikbd_intr();
#define NU_YIELD()	do { if (*(VOL unsigned char *)0xFFFFFC00 & 0x80) \
				ikbd_intr(); } while (0)
/* the 240 Hz clock is pending: holding it off one more period loses a tick */
#define NU_CLKDUE()	(*(VOL unsigned char *)0xFFFFFA0B & 0x20)
#endif

#endif
