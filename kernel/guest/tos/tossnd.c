/*
 * tossnd.c -- the TOS environment's sound pump, a process of starttos.
 * It plays the buffer that the guest's Falcon XBIOS calls (struct
 * tossnd) or STE DMA sound registers describe, straight from ST-RAM,
 * through the host's sound service, and raises the guest's buffer-end
 * interrupt when each pass has been heard, by the clock.  It sleeps
 * until rung: by the cartridge's calls (the doorbell pipe), a control
 * register write (SIGUSR1), or the next pass end or feed while playing.
 */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include "tosio.h"
#include "sndout.h"

#define	LEAD	50		/* ms of samples fed ahead of the clock */
#define	TICK	10		/* ms between feeds */
#define	NPASS	8

static struct sndout so;
static struct tossnd *sd = (struct tossnd *)TOSSND;
static struct tossndio sn;
static int tfd, wake[2];
static unsigned long ram;
static unsigned long sseen;	/* control register writes acted on */

static int playing, ste, loop;
static int done;			/* a single play is all taken */
static int wait;			/* the next pass waits: a new format once the last is heard */
static int reg;				/* the kernel knows this pump */
static unsigned long beg, end, cur;	/* the pass being read */
static int fs, step, off;		/* frame bytes in memory; bytes heard of each, from off */
static unsigned long irq;

/* passes not yet heard: their clock frames, buffer, and interrupt sources */
static struct pass {
	unsigned long	p_at, p_endat;	/* p_endat 0: still being read */
	unsigned long	p_beg, p_end;
	unsigned long	p_irq;
} pq[NPASS];
static int npq;

static unsigned short steclk[4] = { 6258, 12517, 25033, 50066 };

static void
rung(sig)
	int sig;
{
	char b = 1;

	write(wake[1], &b, 1);
}

/* the kernel's half: buffer ends, the STE registers */
static void
ksnd(ends, ctl)
	unsigned long ends;
	long ctl;
{
	sn.sn_ends = ends;
	sn.sn_ctl = ctl;
	sn.sn_pos = ste && playing ? sd->sd_pos : 0;
	if (ioctl(tfd, TOSIOC_SND, &sn) < 0)
		memset((char *)&sn, 0, sizeof sn);
	else
		reg = 1;
}

static unsigned long
reg24(o)
	int o;
{
	return ((unsigned long)sn.sn_reg[o] << 16 | sn.sn_reg[o + 2] << 8 | sn.sn_reg[o + 4]) & ~1L;
}

static void
stop()
{
	if (playing)
		so_flush(&so);
	playing = wait = done = 0;
	npq = 0;
	sd->sd_play = 0;
}

/*
 * The next pass: its buffer and format from the registers or the XBIOS
 * state.  0: begun, 1: waits for the heard ones (a new format), -1: bad.
 */
static int
pass()
{
	unsigned long rate;
	int enc, ch;

	step = 0;
	off = 0;
	if (ste) {
		beg = reg24(3);
		end = reg24(0xf);
		rate = (unsigned long)steclk[sn.sn_reg[0x21] & 3] << 16;
		enc = SNDE_S8;
		ch = sn.sn_reg[0x21] & 0x80 ? 1 : 2;
		fs = ch;
		irq = TSE_TIMERA | TSE_GPIP7;
	} else {
		beg = sd->sd_beg;
		end = sd->sd_end;
		rate = sd->sd_rate;
		irq = sd->sd_irq;
		enc = sd->sd_mode == 1 ? SNDE_S16 : SNDE_S8;
		ch = sd->sd_mode == 2 ? 1 : 2;
		fs = sd->sd_mode == 1 ? 4 * (sd->sd_tracks + 1) : ch;
		if (sd->sd_mode == 1) {
			step = 4;
			off = 4 * (sd->sd_mon > sd->sd_tracks ? 0 : sd->sd_mon);
		}
	}
	if (!step)
		step = fs;
	if (beg >= end || end > ram || end - beg < fs || rate < 4000L << 16)
		return -1;
	if (so.so_hz == 0 || rate != so.so_fmt.f_rate || enc != so.so_fmt.f_enc ||
	    ch != so.so_fmt.f_chans) {
		if (npq > 0)
			return 1;
		so_start(&so, rate, enc, ch);
	}
	if (npq == NPASS)
		return 1;
	cur = beg;
	pq[npq].p_at = so.so_taken;
	pq[npq].p_endat = 0;
	pq[npq].p_beg = beg;
	pq[npq].p_end = end;
	pq[npq].p_irq = irq;
	npq++;
	return 0;
}

static void
start(src)
	int src;
{
	stop();
	ste = src;
	loop = ste ? sn.sn_reg[1] & 2 : sd->sd_ctl & SB_REPEAT;
	so.so_hz = 0;			/* the clock starts with the first pass */
	playing = 1;
	if (pass() != 0)
		stop();
	sd->sd_play = playing && !ste;
}

/* the pass being read is all taken */
static void
taken()
{
	if (end - cur < fs && npq > 0 && pq[npq - 1].p_endat == 0)
		pq[npq - 1].p_endat = so.so_taken;
}

/* frames from ST-RAM to the stream, a lead ahead of the clock */
static void
feed()
{
	static char b[SO_REC];
	long n, k, i;
	char *p;

	n = so_due(&so, LEAD * (long)so.so_hz / 1000);
	while (n > 0 && playing && !done && !wait) {
		if (end - cur < fs) {
			taken();
			if (!loop) {
				done = 1;
				break;
			}
			if ((i = pass()) < 0) {
				stop();
				break;
			}
			wait = i;
			continue;
		}
		k = (end - cur) / fs;
		if (k > n)
			k = n;
		if (step == fs)
			p = (char *)cur;
		else {
			if (k > sizeof b / step)
				k = sizeof b / step;
			for (i = 0; i < k; i++)
				memcpy(b + i * step, (char *)cur + i * fs + off, step);
			p = b;
		}
		so_put(&so, p, k);
		cur += k * fs;
		n -= k;
	}
	if (playing)
		taken();
}

/* passes heard: their interrupts; the last one of a single play ends it */
static void
ends()
{
	unsigned long t = so_now(&so);
	long ctl;

	while (npq > 0 && pq[0].p_endat && (long)(t - pq[0].p_endat) >= 0) {
		sd->sd_ends++;
		ctl = -1;
		if (npq == 1 && done) {
			playing = 0;
			sd->sd_play = 0;
			if (ste)
				ctl = sn.sn_reg[1] & ~1;
		}
		ksnd(pq[0].p_irq, ctl);
		memmove((char *)pq, (char *)(pq + 1), --npq * sizeof *pq);
	}
	if (npq > 0) {
		t = (long)(t - pq[0].p_at) > 0 ? (t - pq[0].p_at) * fs : 0;
		sd->sd_pos = pq[0].p_beg + (t < pq[0].p_end - pq[0].p_beg ? t : 0);
		if (ste)
			ksnd(0L, -1L);
	}
}

/* what the guest changed */
static void
changes()
{
	unsigned long g = sd->sd_gen;

	if (sn.sn_gen != sseen) {
		sseen = sn.sn_gen;
		if (!(sn.sn_reg[1] & 1)) {
			if (playing && ste)
				stop();
		} else if (!playing || !ste)
			start(1);
		else
			loop = sn.sn_reg[1] & 2;
	}
	if (g != sd->sd_seen) {
		if (!(sd->sd_ctl & SB_PLAY)) {
			if (playing && !ste)
				stop();
		} else if (!playing || ste)
			start(0);
		else
			loop = sd->sd_ctl & SB_REPEAT;
		sd->sd_seen = g;
	}
}

void
tossnd(fd, bell, ramsize)
	int fd, bell;
	unsigned long ramsize;
{
	struct sigaction sa;
	struct pollfd pf[2];
	char b[64];
	long ms, k;
	int i;

	tfd = fd;
	ram = ramsize;
	for (i = 3; i < 256; i++)
		if (i != tfd && i != bell)
			close(i);
	if (pipe(wake) < 0)
		return;
	fcntl(bell, F_SETFL, O_NDELAY);
	fcntl(wake[0], F_SETFL, O_NDELAY);
	fcntl(wake[1], F_SETFL, O_NDELAY);
	memset((char *)&sa, 0, sizeof sa);
	sa.sa_handler = rung;
	sigaction(SIGUSR1, &sa, (struct sigaction *)0);
	signal(SIGPIPE, SIG_IGN);
	so_init(&so);
	ksnd(0L, -1L);
	sseen = sn.sn_gen;
	pf[0].fd = bell;
	pf[1].fd = wake[0];
	pf[0].events = pf[1].events = POLLIN;
	for (;;) {
		ms = reg ? -1 : 100;	/* until the guest has entered */
		if (playing) {
			ms = TICK;
			if (npq > 0 && pq[0].p_endat && (k = so_ms(&so, pq[0].p_endat)) < ms)
				ms = k;
		}
		if (poll(pf, 2UL, (int)ms) < 0 && errno != EINTR)
			return;
		if (pf[0].revents & POLLIN)
			read(bell, b, sizeof b);
		else if (pf[0].revents & (POLLHUP | POLLERR | POLLNVAL))
			return;			/* the guest is gone */
		if (pf[1].revents & POLLIN)
			read(wake[0], b, sizeof b);
		if (!reg || pf[1].revents & POLLIN)
			ksnd(0L, -1L);
		changes();
		if (playing && wait) {
			if ((i = pass()) < 0)
				stop();
			else
				wait = i;
		}
		if (playing)
			feed();
		ends();
	}
}
