/*
 * sndd -- the sound service: the session in front is heard, nothing else.
 *
 *   sndd [-f] [-v level] [-r rate] [-8]
 *	-f: stay in the foreground; level 0..7, default 7; Atari: the
 *	output rate (default 24585 Hz, the nearest the hardware has),
 *	-8 its 8-bit mode
 *
 * Serves SNDPATH (sndio.h) on the Mac's chip (/dev/asc, mapped) or the
 * Atari's DMA sound (/dev/dmasnd, written).  One client is heard at a
 * time: the first of the front session's clients with data.  Every other
 * client is drained at the output rate and dropped, so its program keeps
 * time without being heard.  The hardware asks for data by interrupt or
 * poll; a timer runs only while a muted client has data queued.  With
 * neither device the service waits, silent, so init does not respawn it.
 */
#include <sys/types.h>
#include <sys/mman.h>
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

extern int gettimeofday(), chmod(), fattach(), fdetach();

#define NCL	32
#define RING	16384		/* frames per client, 0.74 s */
#define INMAX	4096		/* bytes read from a client at once */
#define MUTEMS	20		/* pacing period while muted data waits */
#define WDMS	300		/* the chip is silent if no interrupt came in this long */
#define SESSMAX	8		/* clients of one user in one session (the Mac uses 7) */
#define LEADMS	100		/* Atari: most queued in the driver */
/* never fill the FIFO: its full and empty states share one interrupt bit */
#define PRIME	(ASC_FIFOLEN - 1)
#define HALF	(ASC_FIFOLEN / 2 - 1)

struct cl {
	int		c_fd;		/* -1 slot free */
	uid_t		c_uid;
	long		c_sess;		/* bound session, -1 none */
	int		c_get, c_n;
	struct sndfmt	c_fmt;
	unsigned long	c_acc;		/* resampler phase */
	unsigned char	c_in[INMAX];	/* read, not yet taken */
	int		c_inoff, c_inlen;
	struct sndrec	c_rec;
	int		c_hlen;		/* header bytes so far */
	unsigned long	c_left;		/* body bytes to come */
	unsigned char	c_body[8];	/* small body, or a partial frame */
	int		c_blen;
	int		c_drain;	/* a drain reply is owed */
	unsigned long	c_heard, c_muted;
	short		c_ring[2 * RING];	/* frames: left, right; last, not cleared */
};

static struct cl cl[NCL];
static struct cl *cur;		/* the client being heard */
static volatile unsigned char *asc;
static int dfd = -1;		/* Atari: /dev/dmasnd */
static struct dmafmt dfmt;
static unsigned long orate = ASC_RATE;	/* output rate, 16.16 */
static int frameus = 45;	/* microseconds per output frame, for pacing */
static long front, fuid;
static int hold;		/* a passthrough guest has the hardware */
static int playing, vol = 7;
static unsigned long fed, nirq, under;
static struct timeval last, irqt;
static unsigned long usacc;	/* pacing remainder, microseconds */

#define ASC(r)	(asc[r])

static int
heard(c)
struct cl *c;
{
	return !hold && c->c_sess == front && (front == 0 || c->c_uid == 0 || c->c_uid == fuid);
}

static void
stop()
{
	if (dfd >= 0) {
		ioctl(dfd, DMA_FLUSH, 0);
		playing = 0;
		return;
	}
	ASC(ASC_MODE) = 0;
	ASC(ASC_IRQMASKA) = 1;
	ASC(ASC_FIFOMODE) = 0x80;
	ASC(ASC_FIFOMODE) = 0;
	playing = 0;
}

/* up to max frames of c into the FIFOs */
static void
push(c, max)
struct cl *c;
int max;
{
	register int i, k = c->c_n < max ? c->c_n : max;

	register short *f;

	for (i = 0; i < k; i++) {
		f = &c->c_ring[2 * c->c_get];
		ASC(ASC_FIFOA + (i & (ASC_FIFOLEN - 1))) = (f[0] >> 8) ^ 0x80;
		ASC(ASC_FIFOB + (i & (ASC_FIFOLEN - 1))) = (f[1] >> 8) ^ 0x80;
		if (++c->c_get == RING)
			c->c_get = 0;
	}
	c->c_n -= k;
	c->c_heard += k;
	fed += k;
}

/* Atari: as many frames of c as the driver takes, in its format */
static void
dpush(c)
register struct cl *c;
{
	static char b[4096];
	register int i, k, g;
	register short *f;
	int fs = dfmt.d_chans * dfmt.d_bits / 8, n;
	char *src = b;

	k = c->c_n < sizeof b / fs ? c->c_n : sizeof b / fs;
	if (dfmt.d_bits == 16) {	/* the ring is already in the driver's format */
		k = c->c_n < RING - c->c_get ? c->c_n : RING - c->c_get;
		src = (char *)&c->c_ring[2 * c->c_get];
	} else for (i = 0, g = c->c_get; i < k; i++) {
		f = &c->c_ring[2 * g];
		if (dfmt.d_chans == 2)
			b[2 * i] = f[0] >> 8, b[2 * i + 1] = f[1] >> 8;
		else
			b[i] = (f[0] + f[1]) >> 9;
		if (++g == RING)
			g = 0;
	}
	if ((n = write(dfd, src, (unsigned)(k * fs))) <= 0)
		return;
	k = n / fs;
	c->c_get = (c->c_get + k) % RING;
	c->c_n -= k;
	c->c_heard += k;
	fed += k;
	gettimeofday(&irqt, (void *)0);
}

static void
start()
{
	if (dfd >= 0) {
		playing = 1;
		gettimeofday(&irqt, (void *)0);
		dpush(cur);
		return;
	}
	ASC(ASC_MODE) = 0;
	ASC(ASC_FIFOMODE) = 0x80;
	ASC(ASC_FIFOMODE) = 0;
	ASC(ASC_CONTROL) = 0x02;
	ASC(ASC_CLOCK) = 0;
	ASC(ASC_VOLUME) = vol << 5;
	ASC(ASC_IRQMASKB) = 1;
	ASC(ASC_MODE) = 1;
	gettimeofday(&irqt, (void *)0);
	push(cur, PRIME);
	ASC(ASC_IRQMASKA) = 0;
	playing = 1;
}

/* FIFO A asked for data */
static void
chipirq(bits)
unsigned long bits;
{
	if (!playing)
		return;
	if (dfd >= 0) {
		if (!(bits & DMA_EVEMPTY))
			return;
		if (cur && cur->c_n > 0) {
			under++;
			dpush(cur);
		} else
			playing = 0;
		return;
	}
	if (bits & ASC_IRQEMPTY) {
		if (cur && cur->c_n > 0) {
			under++;
			push(cur, PRIME);
		} else
			stop();
	} else if ((bits & ASC_IRQHALF) && cur && cur->c_n > 0)
		push(cur, HALF);
}

static void
drop(c)
struct cl *c;
{
	if (c == cur) {
		if (playing)
			stop();
		cur = 0;
	}
	close(c->c_fd);
	c->c_fd = -1;
}

static int
reply(c, cmd, body, len)
struct cl *c;
unsigned long cmd;
char *body;
int len;
{
	char b[sizeof(struct sndrec) + sizeof(struct sndstat)];
	struct sndrec r;

	r.r_cmd = cmd;
	r.r_len = len;
	memcpy(b, (char *)&r, sizeof r);
	if (len)
		memcpy(b + sizeof r, body, len);
	if (write(c->c_fd, b, (unsigned)(sizeof r + len)) != sizeof r + len) {
		drop(c);
		return -1;
	}
	return 0;
}

/* k input frames at p, resampled to the output rate */
static void
frames(c, p, k)
register struct cl *c;
register unsigned char *p;
int k;
{
	unsigned long out = orate >> 8, in = c->c_fmt.f_rate >> 8;
	register unsigned long acc = c->c_acc;
	register short *q = c->c_ring;
	register int put = (c->c_get + c->c_n) & (RING - 1), n = 0, l, r;
	int s = c->c_fmt.f_enc == SNDE_S16 ? 2 : 1, st = c->c_fmt.f_chans == 2;
	int u8 = c->c_fmt.f_enc == SNDE_U8 ? 0x8000 : 0;

	for (; k > 0; k--, p += st ? 2 * s : s) {
		/* to signed 16 bits */
		if (s == 2) {
			l = p[0] << 8 | p[1];
			r = st ? p[2] << 8 | p[3] : l;
		} else {
			l = p[0] << 8 ^ u8;
			r = st ? p[1] << 8 ^ u8 : l;
		}
		l = (short)l;
		r = (short)r;
		for (acc += out; acc >= in; acc -= in) {
			q[2 * put] = l;
			q[2 * put + 1] = r;
			put = (put + 1) & (RING - 1);
			n++;
		}
	}
	c->c_acc = acc;
	c->c_n += n;
}

static int
fsize(c)
struct cl *c;
{
	return c->c_fmt.f_chans * (c->c_fmt.f_enc == SNDE_S16 ? 2 : 1);
}

static void
flush(c)
struct cl *c;
{
	c->c_n = 0;
	if (c == cur && playing)
		stop();
}

/* a complete control record; -1 if the client was dropped */
static int
control(c)
struct cl *c;
{
	struct sndfmt f;
	struct sndstat st;
	long id;

	switch (c->c_rec.r_cmd) {
	case SNDR_FMT:
		if (c->c_rec.r_len != sizeof f)
			break;
		memcpy((char *)&f, (char *)c->c_body, sizeof f);
		if (f.f_rate < (4000L << 16) || f.f_enc < SNDE_U8 || f.f_enc > SNDE_S16 ||
		    f.f_chans < 1 || f.f_chans > 2)
			break;
		c->c_fmt = f;
		c->c_acc = 0;
		return 0;
	case SNDR_DRAIN:
		c->c_drain = 1;
		return 0;
	case SNDR_FLUSH:
		flush(c);
		return 0;
	case SNDR_BIND:
		if (c->c_uid != 0 || c->c_rec.r_len != sizeof id)
			break;
		memcpy((char *)&id, (char *)c->c_body, sizeof id);
		c->c_sess = id;
		if (c == cur && !heard(c))
			flush(c);
		return 0;
	case SNDR_STAT:
		st.ss_sess = c->c_sess;
		st.ss_front = front;
		st.ss_heard = c->c_heard;
		st.ss_muted = c->c_muted;
		st.ss_queued = c->c_n;
		st.ss_fed = fed;
		st.ss_nirq = nirq;
		st.ss_under = under;
		return reply(c, (unsigned long)SNDR_STAT, (char *)&st, (int)sizeof st);
	}
	drop(c);
	return -1;
}

/* take buffered input while the ring has room; stops at a drain */
static void
take(c)
register struct cl *c;
{
	unsigned long most = (orate >> 8) / (c->c_fmt.f_rate >> 8) + 1;
	unsigned char b;
	int fs, k;

	while (c->c_fd >= 0 && c->c_inoff < c->c_inlen && !c->c_drain) {
		if (c->c_hlen < sizeof c->c_rec) {
			((unsigned char *)&c->c_rec)[c->c_hlen++] = c->c_in[c->c_inoff++];
			if (c->c_hlen < sizeof c->c_rec)
				continue;
			c->c_left = c->c_rec.r_len;
			c->c_blen = 0;
			if ((c->c_rec.r_cmd != SNDR_PCM && c->c_left > sizeof c->c_body) ||
			    c->c_left > SNDR_MAX) {
				drop(c);
				return;
			}
		} else if (c->c_rec.r_cmd == SNDR_PCM) {
			/* whole frames at once, as many as the ring takes */
			if (c->c_blen == 0) {
				fs = fsize(c);
				k = c->c_inlen - c->c_inoff;
				if (k > c->c_left)
					k = c->c_left;
				k /= fs;
				if (k > (RING - c->c_n) / most)
					k = (RING - c->c_n) / most;
				if (k > 0) {
					frames(c, c->c_in + c->c_inoff, k);
					c->c_inoff += k * fs;
					c->c_left -= k * fs;
					if (c->c_left == 0)
						c->c_hlen = 0;
					continue;
				}
				if (c->c_n + most > RING)
					return;
			}
			b = c->c_in[c->c_inoff++];
			c->c_left--;
			c->c_body[c->c_blen++] = b;
			if (c->c_blen == fsize(c)) {
				frames(c, c->c_body, 1);
				c->c_blen = 0;
			}
		} else {
			c->c_body[c->c_blen++] = c->c_in[c->c_inoff++];
			c->c_left--;
		}
		if (c->c_left == 0) {
			c->c_hlen = 0;
			if (c->c_rec.r_cmd != SNDR_PCM && control(c) < 0)
				return;
		}
	}
}

/* drop muted clients' data at the chip's rate */
static void
pace()
{
	struct timeval now;
	unsigned long us, n;
	register struct cl *c;

	gettimeofday(&now, (void *)0);
	us = (now.tv_sec - last.tv_sec) * 1000000 + now.tv_usec - last.tv_usec;
	last = now;
	if (us > 2000000)
		us = 2000000;
	usacc += us;
	n = usacc / frameus;
	usacc %= frameus;
	for (c = cl; c < cl + NCL; c++) {
		if (c->c_fd < 0 || c == cur || c->c_n == 0)
			continue;
		if (n >= c->c_n) {
			c->c_muted += c->c_n;
			c->c_n = 0;
		} else {
			c->c_muted += n;
			c->c_get = (c->c_get + n) % RING;
			c->c_n -= n;
		}
	}
}

static void
pick()
{
	register struct cl *c;

	if (cur && (cur->c_fd < 0 || !heard(cur)))
		cur = 0;
	if (cur && (cur->c_n > 0 || playing))
		return;
	for (c = cl; c < cl + NCL; c++)
		if (c->c_fd >= 0 && heard(c) && c->c_n > 0) {
			cur = c;
			return;
		}
}

static void
accept(lfd)
int lfd;
{
	struct strrecvfd r;
	register struct cl *c, *f = 0;
	int same = 0;
	long sess;

	if (ioctl(lfd, I_RECVFD, &r) < 0)
		return;
	sess = front == 0 || r.uid == 0 || r.uid == fuid ? front : -1;
	for (c = cl; c < cl + NCL; c++)
		if (c->c_fd < 0) {
			if (f == 0)
				f = c;
		} else if (c->c_uid == r.uid && c->c_sess == sess)
			same++;
	/* one session cannot take every slot */
	if (f == 0 || (r.uid != 0 && same >= SESSMAX)) {
		close(r.fd);
		return;
	}
	c = f;
	memset((char *)c, 0, (char *)c->c_ring - (char *)c);
	c->c_fd = r.fd;
	c->c_uid = r.uid;
	c->c_sess = sess;
	c->c_fmt.f_rate = SND_DEFRATE;
	c->c_fmt.f_enc = SND_DEFENC;
	c->c_fmt.f_chans = 1;
	fcntl(c->c_fd, F_SETFL, O_NONBLOCK);
}

static void
quit()
{
	fdetach(SNDPATH);
	unlink(SNDPATH);
	exit(0);
}

/* no such hardware */
static int
absent(e)
int e;
{
	return e == ENXIO || e == ENODEV || e == ENOENT;
}

static void
setup(afdp, lfdp, rate, bits)
int *afdp, *lfdp;
long rate;
int bits;
{
	int p[2], fd;

	if ((*afdp = open("/dev/asc", O_RDWR)) < 0 && absent(errno) &&
	    (*afdp = dfd = open("/dev/dmasnd", O_RDWR)) < 0 && absent(errno))
		for (;;)
			pause();
	if (*afdp < 0) {
		perror(dfd < 0 && errno != EBUSY ? "sndd: /dev/asc" : "sndd: /dev/dmasnd");
		exit(1);
	}
	if (dfd >= 0) {
		dfmt.d_rate = rate;
		dfmt.d_bits = bits;
		dfmt.d_chans = 2;
		if (ioctl(dfd, DMA_SETFMT, &dfmt) < 0) {
			perror("sndd: DMA_SETFMT");
			exit(1);
		}
		ioctl(dfd, DMA_SETLIMIT, dfmt.d_rate * dfmt.d_chans * dfmt.d_bits / 8 * LEADMS / 1000);
		ioctl(dfd, DMA_SETVOL, vol);
		fcntl(dfd, F_SETFL, O_NONBLOCK);
		orate = (unsigned long)dfmt.d_rate << 16;
		frameus = 1000000 / dfmt.d_rate;
	} else {
		asc = (volatile unsigned char *)mmap((caddr_t)0, ASC_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED, *afdp, (off_t)0);
		if ((caddr_t)asc == (caddr_t)-1) {
			perror("sndd: mmap");
			exit(1);
		}
	}
	stop();
	if ((fd = open(SNDPATH, O_RDWR | O_CREAT, 0666)) >= 0)
		close(fd);
	chmod(SNDPATH, 0666);
	if (pipe(p) < 0 || ioctl(p[1], I_PUSH, "connld") < 0) {
		perror("sndd: pipe");
		exit(1);
	}
	if (fattach(p[1], SNDPATH) < 0 && (fdetach(SNDPATH), fattach(p[1], SNDPATH) < 0)) {
		perror("sndd: fattach");
		exit(1);
	}
	*lfdp = p[0];
}

int
main(argc, argv)
int argc;
char **argv;
{
	struct pollfd pf[2 + NCL];
	struct cl *pc[2 + NCL];
	struct sndev e;
	struct timeval now;
	int afd, lfd, i, k, n, fg = 0, tmo, bits = 16;
	long rate = 24585;
	register struct cl *c;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-f") == 0)
			fg = 1;
		else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc)
			vol = atoi(argv[++i]) & 7;
		else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
			rate = atol(argv[++i]);
		else if (strcmp(argv[i], "-8") == 0)
			bits = 8;
	setup(&afd, &lfd, rate, bits);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, quit);
	if (!fg) {
		if (fork() != 0)
			exit(0);
		setsid();
	}
	for (c = cl; c < cl + NCL; c++)
		c->c_fd = -1;
	gettimeofday(&last, (void *)0);
	for (;;) {
		pf[0].fd = afd;
		pf[0].events = POLLIN;
		if (dfd >= 0 && playing && cur && cur->c_n > 0)
			pf[0].events |= POLLOUT;
		pf[1].fd = lfd;
		pf[1].events = POLLIN;
		n = 2;
		tmo = playing ? WDMS : -1;
		for (c = cl; c < cl + NCL; c++) {
			if (c->c_fd < 0)
				continue;
			if (c != cur && c->c_n > 0)
				tmo = MUTEMS;
			if (c->c_inoff < c->c_inlen || c->c_drain)
				continue;
			pc[n] = c;
			pf[n].fd = c->c_fd;
			pf[n++].events = POLLIN;
		}
		if (poll(pf, (unsigned long)n, tmo) < 0 && errno != EINTR) {
			perror("sndd: poll");
			exit(1);
		}
		if (pf[0].revents & POLLIN && read(afd, (char *)&e, sizeof e) == sizeof e) {
			nirq = e.se_nirq;
			gettimeofday(&irqt, (void *)0);
			if (e.se_front != front || e.se_fuid != fuid || e.se_hold != hold) {
				if (playing)
					stop();
				cur = 0;
				front = e.se_front;
				fuid = e.se_fuid;
				hold = e.se_hold;
			} else
				chipirq(e.se_irq);
		}
		if (pf[0].revents & POLLOUT && playing && cur)
			dpush(cur);
		if (pf[1].revents & POLLIN)
			accept(lfd);
		for (i = 2; i < n; i++) {
			c = pc[i];
			if (!(pf[i].revents & (POLLIN | POLLHUP)) || c->c_fd < 0)
				continue;
			c->c_inoff = 0;
			if ((k = read(c->c_fd, (char *)c->c_in, INMAX)) <= 0) {
				if (k == 0 || errno != EAGAIN)
					drop(c);
				k = 0;
			}
			c->c_inlen = k;
		}
		if (playing) {
			gettimeofday(&now, (void *)0);
			if ((now.tv_sec - irqt.tv_sec) * 1000L + (now.tv_usec - irqt.tv_usec) / 1000 > WDMS)
				stop();		/* a lost interrupt must not hold a drain forever */
		}
		pace();
		pick();
		for (c = cl; c < cl + NCL; c++)
			if (c->c_fd >= 0)
				take(c);
		for (c = cl; c < cl + NCL; c++)
			if (c->c_fd >= 0 && c->c_drain && c->c_n == 0 && !(c == cur && playing)) {
				c->c_drain = 0;
				if (reply(c, (unsigned long)SNDR_DRAIN, (char *)0, 0) == 0)
					take(c);
			}
		pick();
		if (!playing && cur && cur->c_n > 0)
			start();
	}
}
