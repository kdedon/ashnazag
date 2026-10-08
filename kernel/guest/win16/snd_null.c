/*
 * snd_null.c -- the session's sound for the tests: the clock only, as
 * snd_so.c keeps it without a sound service.  With W16_SND set the
 * samples are appended to that file as given (Windows' own formats),
 * so a test can check what was played.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "w16.h"
#include "snd.h"

static long hz, t0, u0;
static unsigned long taken;
static int bytes;
static FILE *out;

static unsigned long
frames()
{
	struct timeval tv;
	long ds, du;

	gettimeofday(&tv, (void *)0);
	ds = tv.tv_sec - t0;
	du = tv.tv_usec - u0;
	if (du < 0) {
		du += 1000000;
		ds--;
	}
	return ds < 0 ? 0 : ds * hz + du / 1000 * hz / 1000;
}

void
snd_start(rate, bits, chans)
	long rate;
	int bits, chans;
{
	struct timeval tv;

	gettimeofday(&tv, (void *)0);
	t0 = tv.tv_sec;
	u0 = tv.tv_usec;
	hz = rate;
	bytes = chans * (bits == 16 ? 2 : 1);
	taken = 0;
	if (!out && getenv("W16_SND"))
		out = fopen(getenv("W16_SND"), "ab");
}

long
snd_due(lead)
	long lead;
{
	unsigned long t;

	if (!hz)
		return 0;
	t = frames();
	if ((long)(taken - t) < 0)
		taken = t;
	return (long)(t + lead * hz / 1000 - taken);
}

void
snd_put(p, n)
	char *p;
	long n;
{
	if (n <= 0)
		return;
	taken += n;
	if (out) {
		fwrite(p, bytes, n, out);
		fflush(out);
	}
}

unsigned long
snd_now()
{
	return hz ? frames() : 0;
}

unsigned long
snd_taken()
{
	return taken;
}

long
snd_ms(frame)
	unsigned long frame;
{
	unsigned long t = snd_now();

	if (!hz || (long)(frame - t) <= 0)
		return 0;
	return (long)((frame - t) * 1000 / hz) + 1;
}

void
snd_flush()
{
	taken = snd_now();
}
