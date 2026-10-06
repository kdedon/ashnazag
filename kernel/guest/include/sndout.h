/*
 * sndout.h -- an environment's sound stream to the host's sound service
 * (sndio.h), paced by the clock: the stream is fed at most a lead ahead
 * of it.  Without the service, or with a full stream, frames are dropped
 * and time is kept, so a guest's buffer ends still come on time.
 */
#ifndef SNDOUT_H
#define SNDOUT_H

#include "sndio.h"

#define SO_REC		4096		/* bytes per record, header included */

struct sndout {
	int		so_fd;		/* -1: none */
	long		so_retry;	/* seconds at the last failed open */
	struct sndfmt	so_fmt;
	unsigned long	so_hz;		/* whole Hz */
	int		so_fs;		/* frame bytes */
	long		so_t0, so_u0;	/* when frame 0 plays: seconds, microseconds */
	unsigned long	so_taken;	/* frames taken since then */
	char		so_pend[SO_REC];
	int		so_plen, so_poff;	/* a record the stream has not taken */
};

extern void so_init();		/* (so) */
extern void so_start();		/* (so, rate 16.16, SNDE_*, channels): the clock starts now */
extern long so_due();		/* (so, lead frames): frames to take now */
extern long so_ms();		/* (so, frame): milliseconds until it plays, >= 0 */
extern unsigned long so_now();	/* (so): the frame playing */
extern void so_put();		/* (so, bytes, frames) */
extern void so_flush();		/* (so): drop what is queued */

#endif
