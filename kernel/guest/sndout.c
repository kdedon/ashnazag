/*
 * sndout.c -- an environment's sound stream to the host's sound service.
 */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stropts.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "sndout.h"

extern int gettimeofday();

void
so_init(so)
	struct sndout *so;
{
	memset((char *)so, 0, sizeof *so);
	so->so_fd = -1;
	so->so_retry = -1;
}

static void
now(sp, up)
	long *sp, *up;
{
	struct timeval tv;

	gettimeofday(&tv, (void *)0);
	*sp = tv.tv_sec;
	*up = tv.tv_usec;
}

/* the stream; a plain file left at the path is refused; retried once a second */
static int
opened(so)
	struct sndout *so;
{
	long s, u;
	int n;

	if (so->so_fd >= 0)
		return 1;
	now(&s, &u);
	if (s == so->so_retry)
		return 0;
	if ((so->so_fd = open(SNDPATH, O_RDWR | O_NDELAY)) >= 0 &&
	    ioctl(so->so_fd, I_NREAD, &n) < 0) {
		close(so->so_fd);
		so->so_fd = -1;
	}
	if (so->so_fd < 0) {
		so->so_retry = s;
		return 0;
	}
	fcntl(so->so_fd, F_SETFD, 1);
	so->so_plen = 0;
	return 1;
}

static void
lost(so)
	struct sndout *so;
{
	close(so->so_fd);
	so->so_fd = -1;
	so->so_plen = 0;
}

/* the pending record; 0 when it is gone */
static int
push(so)
	struct sndout *so;
{
	int n;

	while (so->so_plen > so->so_poff) {
		n = write(so->so_fd, so->so_pend + so->so_poff, so->so_plen - so->so_poff);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return 1;
		if (n <= 0) {
			lost(so);
			return 0;
		}
		so->so_poff += n;
	}
	so->so_plen = so->so_poff = 0;
	return 0;
}

/* a record: sent, queued while the stream is full, or dropped */
static void
rec(so, cmd, p, n)
	struct sndout *so;
	unsigned long cmd;
	char *p;
	int n;
{
	struct sndrec r;

	if (!opened(so))
		return;
	if (push(so)) {
		if (cmd == SNDR_PCM)
			return;
		if (so->so_poff) {	/* a control record waits for a started one */
			fcntl(so->so_fd, F_SETFL, O_RDWR);
			push(so);
			if (so->so_fd >= 0)
				fcntl(so->so_fd, F_SETFL, O_RDWR | O_NDELAY);
		}
		so->so_plen = so->so_poff = 0;
	}
	if (so->so_fd < 0)
		return;
	r.r_cmd = cmd;
	r.r_len = n;
	memcpy(so->so_pend, (char *)&r, sizeof r);
	memcpy(so->so_pend + sizeof r, p, n);
	so->so_plen = sizeof r + n;
	push(so);
}

void
so_start(so, rate, enc, chans)
	struct sndout *so;
	unsigned long rate;
	int enc, chans;
{
	so->so_fmt.f_rate = rate;
	so->so_fmt.f_enc = enc;
	so->so_fmt.f_chans = chans;
	so->so_hz = rate >> 16;
	so->so_fs = chans * (enc == SNDE_S16 ? 2 : 1);
	now(&so->so_t0, &so->so_u0);
	so->so_taken = 0;
	rec(so, (unsigned long)SNDR_FMT, (char *)&so->so_fmt, (int)sizeof so->so_fmt);
}

unsigned long
so_now(so)
	struct sndout *so;
{
	long s, u, ds, du;

	now(&s, &u);
	ds = s - so->so_t0;
	du = u - so->so_u0;
	if (du < 0) {
		du += 1000000;
		ds--;
	}
	if (ds < 0)
		return 0;
	return ds * so->so_hz + du / 1000 * so->so_hz / 1000;	/* wraps; callers compare differences */
}

long
so_due(so, lead)
	struct sndout *so;
	long lead;
{
	unsigned long t = so_now(so);

	/* late: the frames that should have played are gone; start again from now */
	if ((long)(so->so_taken - t) < 0)
		so->so_taken = t;
	return (long)(t + lead - so->so_taken);
}

long
so_ms(so, frame)
	struct sndout *so;
	unsigned long frame;
{
	unsigned long t = so_now(so);

	if ((long)(frame - t) <= 0 || so->so_hz == 0)
		return 0;
	frame -= t;
	if (frame > 100 * so->so_hz)
		return 100000L;
	return (frame * 1000 + so->so_hz - 1) / so->so_hz;
}

void
so_put(so, p, n)
	struct sndout *so;
	char *p;
	long n;
{
	long k, max = (SO_REC - sizeof(struct sndrec)) / so->so_fs;

	so->so_taken += n;
	for (; n > 0; n -= k, p += k * so->so_fs) {
		k = n < max ? n : max;
		rec(so, (unsigned long)SNDR_PCM, p, (int)(k * so->so_fs));
	}
}

void
so_flush(so)
	struct sndout *so;
{
	rec(so, (unsigned long)SNDR_FLUSH, (char *)0, 0);
}
