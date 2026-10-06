/*
 * sndaux -- the A/UX Sound Manager's devices, /dev/snd, on the sound service.
 *
 *   sndaux [-f]	-f: stay in the foreground
 *
 * The Mac's own Sound Manager (A/UX's toolbox patches) opens one device per
 * synth and drives it with 'w' ioctls and writes; the kernel relays them
 * here (sndio.h, SA_SRV).  Each channel becomes a private SNDPATH stream,
 * opened with the caller's user ID, so the service's rule decides whether
 * it is heard.  Commands are rendered in order into that stream:
 *
 *   samples written, then bufferCmd/soundCmd	FMT at the command's rate, PCM
 *   noteCmd/freqCmd/waitCmd/restCmd (note, wave)	square or wavetable tone, silence
 *   callBackCmd				DRAIN; its reply queues the command
 *						and sends the caller SIGPOLL
 *   quietCmd/flushCmd				what is not yet sent is dropped, FLUSH
 *
 * Long samples: once the first 64 KB play, SIGEMT asks for the rest.
 * A closed channel plays out, then its stream closes.
 */
#include <sys/types.h>
#include <sys/stropts.h>
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sndio.h"

extern int gettimeofday(), seteuid(), kill();

#define NCH	16
#define QSZ	(96 * 1024)	/* records not yet sent to the service */
#define PSZ	(128 * 1024)	/* samples before their command; the last sample */
#define NCB	25		/* callbacks waiting to be fetched */
#define NTAG	64
#define REC	4096		/* PCM bytes per record */
#define CHIPHZ	22254
#define LEADMS	60		/* tone generated ahead of the clock */
#define FIRST	65536		/* sample bytes written before the command */

/* the ioctls, as the toolbox issues them */
#define SND_VOL		0xC0017701
#define SND_HDR		0xC0187702
#define SND_CMD_I	0xC0087703
#define SND_CMD_Q	0xC0087704
#define SND_CALLBACK	0x20007705
#define SND_RESET	0x20007706
#define SND_RAW_CTL	0xC0087707
#define SND_QFULL	0x20007708
#define SND_GET_CB	0xC0087709
#define SND_BUSY	0xC001770A

#define M_NOTE	0
#define M_SAMP	5
#define M_RAW	6

#define G16(p)	((((unsigned char *)(p))[0] << 8) | ((unsigned char *)(p))[1])
#define G32(p)	(((unsigned long)G16(p) << 16) | G16((char *)(p) + 2))

struct tag {
	int		t_cb;		/* 1: a callBackCmd; 0: the channel's close */
	unsigned char	t_cmd[8];
};

struct ch {
	int		c_used;
	int		c_minor;
	int		c_closed;	/* playing out */
	long		c_pid, c_uid;
	int		c_fd;		/* the stream, -1 until needed */
	unsigned char	*c_q;		/* ring of records */
	long		c_qget, c_qn, c_wleft;	/* c_wleft: bytes of the record being sent */
	unsigned char	*c_p;		/* samples or wavetable waiting for their command */
	long		c_pn;
	unsigned char	*c_s;		/* the last sample, for noteCmd on the sampled synth */
	long		c_sn;
	int		c_armed;	/* writes play now */
	unsigned long	c_hlen;		/* SND_HDR: length, rate */
	unsigned long	c_hrate;
	int		c_emt;		/* SIGEMT due once the first part is queued */
	unsigned long	c_raw;		/* SND_RAW_CTL rate, Fixed ratio to the chip */
	struct sndfmt	c_fmt;		/* the format of what is queued; f_rate 0 none */
	struct tag	c_tag[NTAG];	/* drains: queued, the first c_tsent of them sent */
	int		c_tn, c_tsent;
	unsigned char	c_cb[NCB][8];
	int		c_cbn;
	int		c_wait;		/* a writer waits for room */
	unsigned long	c_inc, c_ph;	/* tone: phase increment (8.24 per chip frame), phase */
	int		c_tone, c_amp;
	unsigned char	c_wave[512];
	struct timeval	c_end;		/* when what is queued has played */
	unsigned char	c_in[64];	/* replies from the service */
	int		c_inn;
};

static struct ch ch[NCH];
static int srv;

/* the signal goes out as the caller: a recycled pid of another user is out of reach */
static void
sig(c, n)
struct ch *c;
int n;
{
	if (seteuid((uid_t)c->c_uid) < 0)
		return;
	(void)kill((pid_t)c->c_pid, n);
	(void)seteuid((uid_t)0);
}

static long
msto(t)
struct timeval *t;
{
	struct timeval n;

	gettimeofday(&n, (void *)0);
	return (t->tv_sec - n.tv_sec) * 1000L + (t->tv_usec - n.tv_usec) / 1000;
}

/* frames at the queued rate extend the play-out time */
static void
later(c, frames)
struct ch *c;
unsigned long frames;
{
	struct timeval n;
	unsigned long hz = c->c_fmt.f_rate >> 16, us;

	gettimeofday(&n, (void *)0);
	if (c->c_end.tv_sec < n.tv_sec || (c->c_end.tv_sec == n.tv_sec && c->c_end.tv_usec < n.tv_usec))
		c->c_end = n;
	us = hz ? frames / hz * 1000000 + frames % hz * 1000000 / hz : 0;
	c->c_end.tv_usec += us % 1000000;
	c->c_end.tv_sec += us / 1000000 + c->c_end.tv_usec / 1000000;
	c->c_end.tv_usec %= 1000000;
}

static long
qfree(c)
struct ch *c;
{
	return QSZ - c->c_qn;
}

static void
qput(c, p, n)
register struct ch *c;
char *p;
long n;
{
	long at = (c->c_qget + c->c_qn) % QSZ, k;

	while (n > 0) {
		k = QSZ - at < n ? QSZ - at : n;
		memcpy((char *)c->c_q + at, p, (int)k);
		c->c_qn += k;
		at = (at + k) % QSZ;
		p += k;
		n -= k;
	}
}

static void
rec(c, cmd, p, n)
struct ch *c;
unsigned long cmd;
char *p;
long n;
{
	struct sndrec r;

	r.r_cmd = cmd;
	r.r_len = n;
	qput(c, (char *)&r, (long)sizeof r);
	if (n)
		qput(c, p, n);
}

static void
fmt(c, rate)
struct ch *c;
unsigned long rate;
{
	if (rate < (4000L << 16))
		rate = 4000L << 16;
	if (c->c_fmt.f_rate == rate)
		return;
	c->c_fmt.f_rate = rate;
	c->c_fmt.f_enc = SNDE_U8;
	c->c_fmt.f_chans = 1;
	rec(c, (unsigned long)SNDR_FMT, (char *)&c->c_fmt, (long)sizeof c->c_fmt);
}

/* up to n sample bytes at the current format; how many fit */
static long
pcm(c, p, n)
struct ch *c;
char *p;
long n;
{
	long k, done = 0;

	while (done < n && qfree(c) > REC + 64) {
		k = n - done < REC ? n - done : REC;
		rec(c, (unsigned long)SNDR_PCM, p + done, k);
		done += k;
	}
	later(c, (unsigned long)done);
	return done;
}

/* a Fixed ratio to the chip's rate as a 16.16 rate in Hz */
static unsigned long
ratio(r)
unsigned long r;
{
	double hz = (double)r / 65536.0 * ((double)ASC_RATE / 65536.0);

	return hz > 65535.0 ? 65535L << 16 : (unsigned long)(hz * 65536.0);
}

/* frames of the tone (or silence) at the chip's rate */
static void
tone(c, frames)
register struct ch *c;
unsigned long frames;
{
	unsigned char b[REC];
	register int i, k, s;

	fmt(c, (unsigned long)ASC_RATE);
	while (frames > 0 && qfree(c) > REC + 64) {
		k = frames < REC ? frames : REC;
		for (i = 0; i < k; i++) {
			if (!c->c_tone)
				s = 0;
			else if (c->c_minor == M_NOTE)
				s = c->c_ph & 0x800000 ? 127 : -127;
			else
				s = (int)c->c_wave[(c->c_ph >> 15) & 511] - 128;
			b[i] = 128 + s * c->c_amp / 255;
			c->c_ph = (c->c_ph + c->c_inc) & 0xffffff;
		}
		rec(c, (unsigned long)SNDR_PCM, (char *)b, (long)k);
		later(c, (unsigned long)k);
		frames -= k;
	}
}

static void
drain(c, cb, cmd)
struct ch *c;
int cb;
char *cmd;
{
	if (c->c_tn == NTAG)
		return;
	c->c_tag[c->c_tn].t_cb = cb;
	if (cmd)
		memcpy((char *)c->c_tag[c->c_tn].t_cmd, cmd, 8);
	c->c_tn++;
	rec(c, (unsigned long)SNDR_DRAIN, (char *)0, 0L);
}

/* drop what is not yet sent; the record being sent completes */
static void
flush(c)
struct ch *c;
{
	c->c_qn = c->c_wleft;
	c->c_tn = c->c_tsent;
	c->c_tone = 0;
	c->c_armed = 0;
	c->c_pn = 0;
	c->c_fmt.f_rate = 0;
	rec(c, (unsigned long)SNDR_FLUSH, (char *)0, 0L);
	gettimeofday(&c->c_end, (void *)0);
}

/* the queued sample starts at the command's rate */
static void
arm(c, r)
struct ch *c;
unsigned long r;
{
	fmt(c, r ? ratio(r) : c->c_hrate);
	if (c->c_pn > 0) {
		memcpy((char *)c->c_s, (char *)c->c_p, (int)c->c_pn);
		c->c_sn = c->c_pn;
		(void)pcm(c, (char *)c->c_p, c->c_pn);
		c->c_pn = 0;
	}
	c->c_armed = 1;
	if (c->c_hlen > FIRST && c->c_emt) {
		c->c_emt = 0;
		sig(c, SIGEMT);
	}
}

static void
command(c, p)
register struct ch *c;
unsigned char *p;
{
	int cmd = G16(p), p1 = G16(p + 2);
	unsigned long p2 = G32(p + 4), n;

	n = (unsigned long)p1 * CHIPHZ / 2000;	/* half milliseconds */
	switch (cmd) {
	case 3:		/* quietCmd */
	case 4:		/* flushCmd */
		flush(c);
		break;
	case 13:	/* callBackCmd */
		drain(c, 1, (char *)p);
		break;
	case 10:	/* waitCmd */
		tone(c, n);
		break;
	case 41:	/* restCmd */
		c->c_tone = 0;
		tone(c, n);
		break;
	case 43:	/* ampCmd */
		c->c_amp = p2 & 255;
		break;
	case 40:	/* noteCmd */
	case 42:	/* freqCmd */
		if (c->c_minor == M_SAMP) {
			if (p2 && c->c_sn) {
				fmt(c, ratio(p2));
				(void)pcm(c, (char *)c->c_s, c->c_sn);
			}
			break;
		}
		c->c_inc = p2 & 0xffffff;
		c->c_tone = p2 != 0;
		if (cmd == 40) {
			tone(c, n);
			c->c_tone = 0;
		}
		break;
	case 60:	/* waveTableCmd: its bytes came by write() */
		if (c->c_pn > 0) {
			for (n = 0; n < 512; n++)
				c->c_wave[n] = c->c_p[n * c->c_pn / 512];
			c->c_pn = 0;
		}
		break;
	case 80:	/* soundCmd */
	case 81:	/* bufferCmd */
	case 83:	/* continueCmd */
		if (c->c_minor == M_SAMP || c->c_minor == M_RAW)
			arm(c, p2);
		break;
	}
}

static int
busy(c)
struct ch *c;
{
	return c->c_qn > 0 || c->c_tone || msto(&c->c_end) > 0;
}

static struct ch *
bych(m)
long m;
{
	register struct ch *c;

	for (c = ch; c < ch + NCH; c++)
		if (c->c_used && !c->c_closed && c->c_minor == m)
			return c;
	return 0;
}

static void
release(c)
struct ch *c;
{
	if (c->c_fd >= 0)
		close(c->c_fd);
	free((char *)c->c_q);
	free((char *)c->c_p);
	free((char *)c->c_s);
	c->c_used = 0;
}

static long
open1(q)
struct sareq *q;
{
	register struct ch *c;
	int i;

	for (c = ch; c < ch + NCH; c++)
		if (!c->c_used)
			break;
	if (c == ch + NCH)
		return -EAGAIN;
	memset((char *)c, 0, sizeof *c);
	c->c_q = (unsigned char *)malloc(QSZ);
	c->c_p = (unsigned char *)malloc(PSZ);
	c->c_s = (unsigned char *)malloc(PSZ);
	if (!c->c_q || !c->c_p || !c->c_s) {
		free((char *)c->c_q);
		free((char *)c->c_p);
		free((char *)c->c_s);
		return -ENOMEM;
	}
	c->c_used = 1;
	c->c_minor = q->q_ch;
	c->c_pid = q->q_pid;
	c->c_uid = q->q_uid;
	c->c_fd = -1;
	c->c_amp = 255;
	c->c_raw = 0x10000;
	for (i = 0; i < 512; i++)		/* until a waveTableCmd: a triangle */
		c->c_wave[i] = i < 256 ? i : 511 - i;
	return 0;
}

/* the request's reply: its result and out bytes */
static long
ioc(c, q, b, out)
register struct ch *c;
struct sareq *q;
unsigned char *b;
int *out;
{
	register struct ch *o;
	long m;

	*out = 0;
	switch ((unsigned long)q->q_cmd) {
	case SND_VOL:
	case SND_RESET:
		return 0;
	case SND_QFULL:
		return qfree(c) < QSZ / 4;
	case SND_BUSY:
		return busy(c);
	case SND_CALLBACK:
		for (m = 0, o = ch; o < ch + NCH; o++)
			if (o->c_used && !o->c_closed && o->c_cbn > 0 && o->c_minor < 32)
				m |= 1L << o->c_minor;
		return m;
	case SND_GET_CB:
		if (c->c_cbn == 0)
			return -EAGAIN;
		memcpy((char *)b, (char *)c->c_cb[0], 8);
		memmove((char *)c->c_cb[0], (char *)c->c_cb[1], (c->c_cbn - 1) * 8);
		c->c_cbn--;
		*out = 8;
		return 0;
	case SND_HDR:
		if (q->q_len < 22)
			return -EINVAL;
		c->c_hlen = G32(b + 4);
		c->c_hrate = G32(b + 8);
		c->c_armed = 0;
		c->c_pn = 0;
		c->c_emt = 1;
		return 0;
	case SND_RAW_CTL:
		c->c_raw = G32(b);
		fmt(c, ratio(c->c_raw));
		c->c_armed = 1;
		return 0;
	case SND_CMD_I:
	case SND_CMD_Q:
		if (q->q_len < 8)
			return -EINVAL;
		command(c, b);
		return 0;
	}
	return -EINVAL;
}

static void
answer(q, ret, b, n)
struct sareq *q;
long ret;
char *b;
int n;
{
	char o[sizeof(struct sarep) + SA_ARGMAX];
	struct sarep p;

	p.p_seq = q->q_seq;
	p.p_ch = q->q_ch;
	p.p_ret = ret;
	p.p_len = n;
	memcpy(o, (char *)&p, sizeof p);
	if (n)
		memcpy(o + sizeof p, b, n);
	(void)write(srv, o, (unsigned)(sizeof p + n));
}

static void
request()
{
	static unsigned char b[sizeof(struct sareq) + SA_MAX];
	struct sareq q;
	register struct ch *c;
	unsigned char *d = b + sizeof q;
	long r = 0;
	int n, out = 0;

	if ((n = read(srv, (char *)b, sizeof b)) < (int)sizeof q)
		return;
	memcpy((char *)&q, (char *)b, sizeof q);
	if (q.q_op == SAQ_OPEN) {
		answer(&q, q.q_ch == SA_RESET ? 0L : open1(&q), (char *)0, 0);
		return;
	}
	if ((c = bych(q.q_ch)) == 0) {
		answer(&q, q.q_op == SAQ_CLOSE || q.q_ch == SA_RESET ? 0L : (long)-EIO, (char *)0, 0);
		return;
	}
	switch (q.q_op) {
	case SAQ_CLOSE:
		/* plays out; after the drain the stream closes */
		c->c_closed = 1;
		c->c_tone = 0;
		c->c_cbn = 0;
		if (c->c_fd >= 0 || c->c_qn > 0)
			drain(c, 0, (char *)0);
		else
			release(c);
		break;
	case SAQ_WRITE:
		if (c->c_armed)
			r = pcm(c, (char *)d, q.q_len);
		else {
			r = PSZ - c->c_pn < q.q_len ? PSZ - c->c_pn : q.q_len;
			memcpy((char *)c->c_p + c->c_pn, (char *)d, (int)r);
			c->c_pn += r;
			if (r == 0 && q.q_len > 0) {	/* no SPACE would ever come */
				answer(&q, (long)-ENOSPC, (char *)0, 0);
				return;
			}
		}
		c->c_wait = r < q.q_len;
		break;
	case SAQ_IOCTL:
		r = ioc(c, &q, d, &out);
		break;
	}
	answer(&q, r, (char *)d, out);
}

/* the stream, opened as the caller so the service binds it as the caller */
static int
stream(c)
struct ch *c;
{
	if (c->c_fd >= 0)
		return 0;
	if (seteuid((uid_t)c->c_uid) < 0)
		return -1;
	c->c_fd = open(SNDPATH, O_RDWR);
	(void)seteuid((uid_t)0);
	if (c->c_fd < 0)
		return -1;
	fcntl(c->c_fd, F_SETFL, O_NDELAY);
	return 0;
}

static void
send(c)
register struct ch *c;
{
	struct sndrec r;
	unsigned char *h = (unsigned char *)&r;
	long k, i;

	if (c->c_qn > 0 && stream(c) < 0) {
		/* no service: dropped, the callbacks still come */
		c->c_qn = c->c_wleft = 0;
		for (i = c->c_tsent; i < c->c_tn; i++)
			if (c->c_tag[i].t_cb && c->c_cbn < NCB)
				memcpy((char *)c->c_cb[c->c_cbn++], (char *)c->c_tag[i].t_cmd, 8);
		if (c->c_tn > c->c_tsent && !c->c_closed)
			sig(c, SIGPOLL);
		c->c_tn = c->c_tsent;
		if (c->c_closed && c->c_tn == 0)
			release(c);
		return;
	}
	while (c->c_qn > 0) {
		if (c->c_wleft == 0) {
			for (i = 0; i < sizeof r; i++)
				h[i] = c->c_q[(c->c_qget + i) % QSZ];
			c->c_wleft = sizeof r + r.r_len;
			if (r.r_cmd == SNDR_DRAIN)
				c->c_tsent++;
		}
		k = QSZ - c->c_qget;
		if (k > c->c_wleft)
			k = c->c_wleft;
		if ((k = write(c->c_fd, (char *)c->c_q + c->c_qget, (unsigned)k)) <= 0)
			break;
		c->c_qget = (c->c_qget + k) % QSZ;
		c->c_qn -= k;
		c->c_wleft -= k;
	}
	if (c->c_wait && qfree(c) >= QSZ / 2) {
		struct sarep p;

		c->c_wait = 0;
		p.p_seq = 0;
		p.p_ch = c->c_minor;
		p.p_ret = SAP_SPACE;
		p.p_len = 0;
		(void)write(srv, (char *)&p, sizeof p);
	}
}

/* DRAIN replies: callbacks, or a closed channel's end */
static void
replies(c)
register struct ch *c;
{
	struct sndrec r;
	int k, cb = 0;

	if ((k = read(c->c_fd, (char *)c->c_in + c->c_inn, sizeof c->c_in - c->c_inn)) <= 0) {
		if (k == 0 || errno != EAGAIN) {
			close(c->c_fd);
			c->c_fd = -1;
			c->c_qn = c->c_wleft = 0;
			c->c_tn = c->c_tsent = 0;
			if (c->c_closed)
				release(c);
		}
		return;
	}
	c->c_inn += k;
	while (c->c_inn >= sizeof r) {
		memcpy((char *)&r, (char *)c->c_in, sizeof r);
		if (c->c_inn < sizeof r + r.r_len)
			break;
		c->c_inn -= sizeof r + r.r_len;
		memmove((char *)c->c_in, (char *)c->c_in + sizeof r + r.r_len, c->c_inn);
		if (r.r_cmd != SNDR_DRAIN || c->c_tsent == 0)
			continue;
		if (!c->c_tag[0].t_cb) {
			release(c);
			return;
		}
		if (c->c_cbn < NCB)
			memcpy((char *)c->c_cb[c->c_cbn++], (char *)c->c_tag[0].t_cmd, 8);
		cb = 1;
		memmove((char *)&c->c_tag[0], (char *)&c->c_tag[1], (c->c_tn - 1) * sizeof c->c_tag[0]);
		c->c_tn--;
		c->c_tsent--;
	}
	if (cb && !c->c_closed)
		sig(c, SIGPOLL);
}

int
main(argc, argv)
int argc;
char **argv;
{
	struct pollfd pf[1 + NCH];
	struct ch *pc[1 + NCH];
	register struct ch *c;
	int i, n, tmo;
	long ms;

	if ((srv = open("/dev/snd/srv", O_RDWR)) < 0) {
		perror("sndaux: /dev/snd/srv");
		exit(1);
	}
	signal(SIGPIPE, SIG_IGN);
	if (argc < 2 || strcmp(argv[1], "-f") != 0) {
		if (fork() != 0)
			exit(0);
		setsid();
	}
	for (;;) {
		pf[0].fd = srv;
		pf[0].events = POLLIN;
		n = 1;
		tmo = -1;
		for (c = ch; c < ch + NCH; c++) {
			if (!c->c_used)
				continue;
			/* a held tone: keep LEADMS of it queued */
			if (c->c_tone && !c->c_closed) {
				if ((ms = msto(&c->c_end)) < LEADMS)
					tone(c, (unsigned long)(LEADMS - (ms > 0 ? ms : 0)) * CHIPHZ / 1000);
				tmo = LEADMS / 3;
			}
			send(c);
			if (!c->c_used || c->c_fd < 0)
				continue;
			pc[n] = c;
			pf[n].fd = c->c_fd;
			pf[n++].events = c->c_qn > 0 ? POLLIN | POLLOUT : POLLIN;
		}
		if (poll(pf, (unsigned long)n, tmo) < 0) {
			if (errno == EINTR)
				continue;
			perror("sndaux: poll");
			exit(1);
		}
		if (pf[0].revents & POLLIN)
			request();
		for (i = 1; i < n; i++)
			if (pc[i]->c_used && pc[i]->c_fd == pf[i].fd &&
			    pf[i].revents & (POLLIN | POLLHUP | POLLERR))
				replies(pc[i]);
	}
}
