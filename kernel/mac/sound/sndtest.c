/*
 * sndtest -- a tone through the sound service.
 *
 *   sndtest [-r rate] [-s secs] [freq]	default 22050 Hz, 2 s, 440 Hz
 *
 * Plays a 16-bit stereo sine, waits until it has been heard, and prints
 * the time that took and the service's counters for this client.
 */
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sndio.h"

extern int gettimeofday();

/* sin(x), x = 0..pi/2 in 64 steps, Q15 */
static short q[65] = {
0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512,
10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868,
19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319,
26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767
};

/* one of 256 steps per period */
static int
sine(i)
int i;
{
	i &= 255;
	if (i < 64)
		return q[i];
	if (i < 128)
		return q[128 - i];
	if (i < 192)
		return -q[i - 128];
	return -q[256 - i];
}

static int
put(fd, cmd, body, len)
int fd;
unsigned long cmd;
char *body;
int len;
{
	struct sndrec r;

	r.r_cmd = cmd;
	r.r_len = len;
	if (write(fd, (char *)&r, sizeof r) != sizeof r)
		return -1;
	return len == 0 || write(fd, body, (unsigned)len) == len ? 0 : -1;
}

/* the reply to cmd, its body into b */
static int
get(fd, cmd, b, len)
int fd;
unsigned long cmd;
char *b;
int len;
{
	struct sndrec r;

	if (read(fd, (char *)&r, sizeof r) != sizeof r || r.r_cmd != cmd || r.r_len != len)
		return -1;
	return len == 0 || read(fd, b, (unsigned)len) == len ? 0 : -1;
}

int
main(argc, argv)
int argc;
char **argv;
{
	static short buf[2 * 2048];
	struct sndfmt f;
	struct sndstat st;
	struct timeval t0, t1;
	unsigned long ph = 0, inc;
	long rate = 22050, freq = 440, n, k, i;
	int fd, secs = 2, c;

	while ((c = getopt(argc, argv, "r:s:")) != -1)
		switch (c) {
		case 'r': rate = atol(optarg); break;
		case 's': secs = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: sndtest [-r rate] [-s secs] [freq]\n");
			return 2;
		}
	if (optind < argc)
		freq = atol(argv[optind]);
	if ((fd = open(SNDPATH, O_RDWR)) < 0) {
		perror("sndtest: " SNDPATH);
		return 1;
	}
	f.f_rate = (unsigned long)rate << 16;
	f.f_enc = SNDE_S16;
	f.f_chans = 2;
	/* 256 steps per period, in 24.8 */
	inc = (unsigned long)(freq * 65536L / rate) << 8;
	gettimeofday(&t0, (void *)0);
	if (put(fd, (unsigned long)SNDR_FMT, (char *)&f, (int)sizeof f) < 0)
		goto bad;
	for (n = rate * secs; n > 0; n -= k) {
		k = n < 2048 ? n : 2048;
		for (i = 0; i < k; i++, ph += inc)
			buf[2 * i] = buf[2 * i + 1] = sine((int)(ph >> 16)) / 2;
		if (put(fd, (unsigned long)SNDR_PCM, (char *)buf, (int)(4 * k)) < 0)
			goto bad;
	}
	if (put(fd, (unsigned long)SNDR_DRAIN, (char *)0, 0) < 0 ||
	    get(fd, (unsigned long)SNDR_DRAIN, (char *)0, 0) < 0 ||
	    put(fd, (unsigned long)SNDR_STAT, (char *)0, 0) < 0 ||
	    get(fd, (unsigned long)SNDR_STAT, (char *)&st, (int)sizeof st) < 0)
		goto bad;
	gettimeofday(&t1, (void *)0);
	printf("sndtest: %ld Hz %d s in %ld ms; heard %lu muted %lu fed %lu irq %lu under %lu\n",
	    freq, secs, (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000,
	    st.ss_heard, st.ss_muted, st.ss_fed, st.ss_nirq, st.ss_under);
	return 0;
bad:
	perror("sndtest");
	return 1;
}
