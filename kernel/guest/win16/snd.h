/*
 * snd.h -- the session's sound output, for our wave driver (mmdrv.c):
 * snd_so.c on the host's sound service (sndout.h), snd_null.c keeping
 * time only (the tests).  One stream; samples as Windows has them
 * (8-bit unsigned, 16-bit signed little-endian), converted here.
 */
#ifndef SND_H
#define SND_H

extern void snd_start();		/* (Hz, bits 8 or 16, channels): the clock starts now */
extern long snd_due();			/* (lead ms): frames to give now to stay that far ahead */
extern void snd_put();			/* (samples, frames) */
extern unsigned long snd_now();		/* the frame playing, counted from the start */
extern unsigned long snd_taken();	/* frames given since the start */
extern long snd_ms();			/* (frame): milliseconds until it plays, >= 0 */
extern void snd_flush();		/* drop what is queued; the clock goes on */

#endif
