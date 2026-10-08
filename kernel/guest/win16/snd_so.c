/*
 * snd_so.c -- the session's sound on the host's sound service, through
 * sndout (kernel/guest/sndout.c), as the TOS and Amiga environments
 * play theirs: 8-bit samples as they are (offset binary), 16-bit ones
 * swapped to big-endian.  Without the service, time is kept and the
 * samples dropped, so buffers still end on time.
 */

#include <string.h>
#include "w16.h"
#include "snd.h"
#include "sndout.h"

static struct sndout so;
static int inited, bytes;		/* bytes a frame */
static int bits16;

void
snd_start(hz, bits, chans)
	long hz;
	int bits, chans;
{
	if (!inited) {
		so_init(&so);
		inited = 1;
	}
	bits16 = bits == 16;
	bytes = chans * (bits16 ? 2 : 1);
	so_start(&so, (unsigned long)hz << 16, bits16 ? SNDE_S16 : SNDE_U8, chans);
}

long
snd_due(lead)
	long lead;
{
	return inited && so.so_hz ? so_due(&so, lead * (long)so.so_hz / 1000) : 0;
}

void
snd_put(p, n)
	char *p;
	long n;
{
	char buf[2048];
	long k, i, max = sizeof buf / bytes;

	if (!inited || n <= 0)
		return;
	if (!bits16) {
		so_put(&so, p, n);
		return;
	}
	for (; n > 0; n -= k, p += k * bytes) {
		k = n < max ? n : max;
		for (i = 0; i < k * bytes; i += 2) {
			buf[i] = p[i + 1];
			buf[i + 1] = p[i];
		}
		so_put(&so, buf, k);
	}
}

unsigned long
snd_now()
{
	return inited ? so_now(&so) : 0;
}

unsigned long
snd_taken()
{
	return inited ? so.so_taken : 0;
}

long
snd_ms(frame)
	unsigned long frame;
{
	return inited ? so_ms(&so, frame) : 0;
}

void
snd_flush()
{
	if (inited)
		so_flush(&so);
}
