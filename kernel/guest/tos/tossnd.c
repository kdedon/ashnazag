/*
 * tossnd.c -- the TOS environment's sound pump, a process of starttos.
 * It plays the buffer that the guest's Falcon XBIOS calls (struct
 * tossnd) describe, straight from ST-RAM, through the host's sound
 * service, and raises the guest's buffer-end interrupt when each pass
 * has been heard, by the clock.  It sleeps until rung by the
 * cartridge's calls (the doorbell pipe), or the next pass end or feed
 * while playing.  It also plays the YM2149 as the XBIOS left it (struct
 * tosym) on a stream of its own, running Dosound sequences at 50 Hz.
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
static int tfd;
static unsigned long ram;

static int playing, loop;
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

#define	YMRATE	22050
#define	YMTICK	(YMRATE / 50)	/* frames between sequence steps */
#define	YMIDLE	YMRATE		/* silent frames before the stream stops */

extern unsigned long tos_romlo, tos_romsz;
static struct sndout yo;
static struct tosym *ym = (struct tosym *)TOSYM;
static int yon;				/* streaming */
static long yquiet, ytick;		/* silent frames; frames to the next step */
static unsigned long ytgen, yegen, yseq;
static int ydelay, ytmp;
static unsigned long yph[3], ynph, yeph, ylfsr = 1;
static int epos, eatt, ehold, elev;

/* output level of each 4-bit volume, 3 dB apart */
static short yvol[16] = {
	0, 78, 110, 156, 221, 313, 442, 625, 884, 1250, 1768, 2500, 3536, 5000, 7071, 10000
};

/* a byte of a Dosound sequence; -1 outside ST-RAM and the ROM */
static int
yb(a)
	unsigned long a;
{
	return a < ram || a - tos_romlo < tos_romsz ? *(unsigned char *)a : -1;
}

static void
yset(r, v)
	int r, v;
{
	if (r > 15)
		return;
	ym->ym_reg[r] = v;
	if (r == 13)
		ym->ym_egen++;
}

/* one 50 Hz step of the sequence, as TOS's own Timer C handler runs it */
static void
ystep()
{
	unsigned long c = yseq;
	int in, d, r, i;

	if (ym->ym_tgen != ytgen) {
		ytgen = ym->ym_tgen;
		c = ym->ym_table;
		ydelay = 0;
	}
	if (c == 0)
		return;
	if (ydelay) {
		ydelay--;
		return;
	}
	for (i = 0; i < 64 && (in = yb(c)) >= 0 && in <= 0x80; i++, c += 2) {
		if ((d = yb(c + 1)) < 0)
			break;
		if (in == 0x80)
			ytmp = d;
		else
			yset(in, in == 7 ? (ym->ym_reg[7] & 0xc0) | (d & 0x3f) : d);
	}
	if (in == 0x81 && (r = yb(c + 1)) >= 0 && (d = yb(c + 2)) >= 0 && (i = yb(c + 3)) >= 0) {
		ytmp = (ytmp + d) & 255;
		yset(r, ytmp);
		if (ytmp == i)
			c += 4;
	} else if (in > 0x81 && (d = yb(c + 1)) >= 0) {
		ydelay = d;
		c = d ? c + 2 : 0;
	} else
		c = 0;
	yseq = ym->ym_seq = c;
}

/* phase step for f2/2/den Hz; integer only, as an F-line here would go to the guest */
static unsigned long
ystepof(f2, den)
	unsigned long f2;
	unsigned den;
{
	unsigned long long s = ((unsigned long long)f2 << 31) / ((unsigned long)(den ? den : 1) * YMRATE);

	return s >= 0xffffffffUL ? 0xffffffffL : (unsigned long)s;
}

/* n frames of the chip into b; 0 if all were silent */
static int
ysynth(b, n)
	short *b;
	int n;
{
	unsigned char *rg = ym->ym_reg;
	unsigned long ts[3], ns, es;
	int c, i, v, lv, any = 0, bit;

	if (ym->ym_egen != yegen) {
		yegen = ym->ym_egen;
		epos = ehold = 0;
		eatt = rg[13] & 4;
	}
	for (c = 0; c < 3; c++)
		ts[c] = ystepof(250000L, rg[2 * c] | (rg[2 * c + 1] & 15) << 8);
	ns = ystepof(250000L, rg[6] & 31);
	es = ystepof(15625L, rg[11] | rg[12] << 8);
	for (i = 0; i < n; i++) {
		if ((ynph += ns) < ns) {
			bit = (ylfsr ^ ylfsr >> 3) & 1;
			ylfsr = ylfsr >> 1 | (unsigned long)bit << 16;
		}
		if (!ehold && (yeph += es) < es && ++epos > 15) {
			if (!(rg[13] & 8) || rg[13] & 1) {
				ehold = 1;
				elev = rg[13] & 8 && (eatt != 0) ^ ((rg[13] & 2) != 0) ? 15 : 0;
			} else {
				epos = 0;
				if (rg[13] & 2)
					eatt = !eatt;
			}
		}
		for (v = c = 0; c < 3; c++) {
			yph[c] += ts[c];
			lv = rg[8 + c] & 16 ? ehold ? elev : eatt ? epos : 15 - epos : rg[8 + c] & 15;
			if (lv == 0)
				continue;
			any = 1;
			if ((yph[c] >> 31 | rg[7] >> c) & (ylfsr | rg[7] >> (c + 3)) & 1)
				v += yvol[lv];
		}
		b[i] = v;
	}
	return any;
}

/* the YM stream: sequence steps and samples a lead ahead of its clock */
static void
yfeed()
{
	static short b[(SO_REC - 64) / 2];
	long n, k;

	if (!yon) {
		if (ym->ym_tgen == ytgen && yseq == 0 && !ysynth(b, 1))
			return;
		so_start(&yo, (unsigned long)YMRATE << 16, SNDE_S16, 1);
		yon = 1;
		yquiet = ytick = 0;
	}
	for (n = so_due(&yo, LEAD * (long)YMRATE / 1000); n > 0; n -= k) {
		if (ytick <= 0) {
			ystep();
			ytick = YMTICK;
		}
		k = n < ytick ? n : ytick;
		if (k > sizeof b / sizeof b[0])
			k = sizeof b / sizeof b[0];
		if (ysynth(b, (int)k) || yseq)
			yquiet = 0;
		else if ((yquiet += k) > YMIDLE) {
			yon = 0;
			return;
		}
		so_put(&yo, (char *)b, k);
		ytick -= k;
	}
}

/* the kernel's half: buffer ends */
static void
ksnd(ends)
	unsigned long ends;
{
	sn.sn_ends = ends;
	if (ioctl(tfd, TOSIOC_SND, &sn) == 0)
		reg = 1;
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
 * The next pass: its buffer and format from the XBIOS state.  0: begun,
 * 1: waits for the heard ones (a new format), -1: bad.
 */
static int
pass()
{
	unsigned long rate;
	int enc, ch;

	step = 0;
	off = 0;
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
start()
{
	stop();
	loop = sd->sd_ctl & SB_REPEAT;
	so.so_hz = 0;			/* the clock starts with the first pass */
	playing = 1;
	if (pass() != 0)
		stop();
	sd->sd_play = playing;
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

	while (npq > 0 && pq[0].p_endat && (long)(t - pq[0].p_endat) >= 0) {
		sd->sd_ends++;
		if (npq == 1 && done) {
			playing = 0;
			sd->sd_play = 0;
		}
		ksnd(pq[0].p_irq);
		memmove((char *)pq, (char *)(pq + 1), --npq * sizeof *pq);
	}
	if (npq > 0) {
		t = (long)(t - pq[0].p_at) > 0 ? (t - pq[0].p_at) * fs : 0;
		sd->sd_pos = pq[0].p_beg + (t < pq[0].p_end - pq[0].p_beg ? t : 0);
	}
}

/* what the guest changed */
static void
changes()
{
	unsigned long g = sd->sd_gen;

	if (g != sd->sd_seen) {
		if (!(sd->sd_ctl & SB_PLAY)) {
			if (playing)
				stop();
		} else if (!playing)
			start();
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
	struct pollfd pf[1];
	char b[64];
	long ms, k;
	int i;

	tfd = fd;
	ram = ramsize;
	for (i = 3; i < 256; i++)
		if (i != tfd && i != bell)
			close(i);
	fcntl(bell, F_SETFL, O_NDELAY);
	signal(SIGPIPE, SIG_IGN);
	so_init(&so);
	so_init(&yo);
	ksnd(0L);
	pf[0].fd = bell;
	pf[0].events = POLLIN;
	for (;;) {
		ms = reg ? -1 : 100;	/* until the guest has entered */
		if (playing) {
			ms = TICK;
			if (npq > 0 && pq[0].p_endat && (k = so_ms(&so, pq[0].p_endat)) < ms)
				ms = k;
		}
		if (yon)
			ms = TICK;
		if (poll(pf, 1UL, (int)ms) < 0 && errno != EINTR)
			return;
		if (pf[0].revents & POLLIN)
			read(bell, b, sizeof b);
		else if (pf[0].revents & (POLLHUP | POLLERR | POLLNVAL))
			return;			/* the guest is gone */
		if (!reg)
			ksnd(0L);
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
		yfeed();
	}
}
