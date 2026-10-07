/*
 * DaynaPORT SCSI/Link (or an SD-card SCSI emulator acting as one):
 * probe, bring-up, one-frame transmit and polled receive over the SCSI
 * core.  STREAMS-free; builds on the host against a simulated bus
 * (FS_HOST).
 */
#ifndef DP_H
#define DP_H

#include "scsi.h"

#define DP_MINFRAME	60		/* without CRC */
#define DP_MAXFRAME	1514		/* without CRC */
#define DP_RXASK	3072		/* two headers + largest frames + CRC */
#define DP_RXCTL	0xc0		/* multi-packet read (0x80: one frame) */
#define DP_SETTLE	30		/* ticks the adapter needs after ENABLE */
#define DP_MAXERRS	3		/* failed commands in a row: re-enable */
#define DP_NMC		16		/* multicast addresses sent to the adapter */

#define DPC_READ	0x08
#define DPC_STATS	0x09
#define DPC_WRITE	0x0a
#define DPC_MODE	0x0c
#define DPC_ADDMC	0x0d
#define DPC_ENABLE	0x0e

struct dp_stats {
	unsigned long	ipackets, ierrors, opackets, oerrors;
	unsigned long	polls, empty, rxbad, wedged, reinit, cmderr, busreset;
	unsigned long	nomem;
};

struct dp_softc {
	int		id;		/* SCSI ID, -1 none */
	unsigned char	ea[6];
	unsigned char	rev[4];		/* INQUIRY firmware revision */
	int		running;	/* enabled and address known */
	int		errs;		/* failed commands in a row */
	int		reinit;		/* disable + enable due */
	int		bcast;		/* broadcast-mode command due */
	long		settle;		/* no commands before this tick */
	int		nmc;		/* addresses in mc[] */
	int		mcsent;		/* of them, sent */
	unsigned char	mc[DP_NMC][6];
	unsigned char	txbuf[DP_MAXFRAME];
	unsigned char	rxbuf[DP_RXASK];
	struct dp_stats	st;
};

int	dp_probe();	/* (sc) 0 found, -1 none */
int	dp_init();	/* (sc) 0 or -1; may sleep */
void	dp_stop();	/* (sc) */
int	dp_ready();	/* (sc) 1 when frames may move now */
int	dp_send();	/* (sc, len) send sc->txbuf; 0 or -1 */
int	dp_recv();	/* (sc) frames passed to dp_input, or -1 */
int	dp_addmc();	/* (sc, addr) 0, or -1 when full */

/* supplied by the caller */
void	dp_input();	/* (sc, frame, len without CRC) */

#ifdef FS_HOST
#include <strings.h>
extern long sim_ticks;
void sim_pause();
#define DP_NOW()	sim_ticks
#define DP_PAUSE()	sim_pause()
#else
extern long lbolt;
extern void delay(), bcopy(), bzero();
#define DP_NOW()	lbolt
#define DP_PAUSE()	delay(1)
#endif

#endif
