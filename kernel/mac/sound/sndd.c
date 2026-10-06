/*
 * sndd -- the sound service: the session in front is heard, nothing else.
 *
 *   sndd [-f] [-v level]	-f: stay in the foreground; level 0..7, default 7
 *
 * Maps the chip through /dev/asc and serves SNDPATH (sndio.h).  One
 * client is heard at a time: the first of the front session's clients
 * with data.  Every other client is drained at the chip's rate and
 * dropped, so its program keeps time without being heard.  The chip
 * asks for data by interrupt; a timer runs only while a muted client
 * has data queued.
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
#define FRAMEUS	45		/* microseconds per chip frame, for pacing */
#define MUTEMS	20		/* pacing period while muted data waits */
#define WDMS	300		/* the chip is silent if no interrupt came in this long */
#define SESSMAX	8		/* clients of one user in one session (the Mac uses 7) */
/* never fill the FIFO: its full and empty states share one interrupt bit */
#define PRIME	(ASC_FIFOLEN - 1)
#define HALF	(ASC_FIFOLEN / 2 - 1)

struct cl {
	int		c_fd;		/* -1 slot free */
	uid_t		c_uid;
	long		c_sess;		/* bound session, -1 none */
	unsigned char	c_ring[2 * RING];	/* frames: left, right */
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
};

static struct cl cl[NCL];
static struct cl *cur;		/* the client being heard */
static volatile unsigned char *asc;
static long front, fuid;
static int playing, vol = 7;
static unsigned long fed, nirq, under;
static struct timeval last, irqt;
static unsigned long usacc;	/* pacing remainder, microseconds */

#define ASC(r)	(asc[r])

static int
heard(c)
struct cl *c;
{
	return c->c_sess == front && (front == 0 || c->c_uid == 0 || c->c_uid == fuid);
}

static void
stop()
{
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
	register unsigned char *f;

	for (i = 0; i < k; i++) {
		f = &c->c_ring[2 * c->c_get];
		ASC(ASC_FIFOA + (i & (ASC_FIFOLEN - 1))) = f[0];
		ASC(ASC_FIFOB + (i & (ASC_FIFOLEN - 1))) = f[1];
		if (++c->c_get == RING)
			c->c_get = 0;
	}
	c->c_n -= k;
	c->c_heard += k;
	fed += k;
}

static void
start()
{
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

static void
emit(c, l, r)
register struct cl *c;
int l, r;
{
	register unsigned char *f = &c->c_ring[2 * ((c->c_get + c->c_n) % RING)];

	f[0] = l;
	f[1] = r;
	c->c_n++;
}

/* one input frame, resampled to the chip's rate */
static void
frame(c, p)
register struct cl *c;
register unsigned char *p;
{
	unsigned long out = (unsigned long)ASC_RATE >> 8, in = c->c_fmt.f_rate >> 8;
	int l, r, s = c->c_fmt.f_enc == SNDE_S16 ? 2 : 1;

	l = p[0];
	r = c->c_fmt.f_chans == 2 ? p[s] : l;
	if (c->c_fmt.f_enc != SNDE_U8) {
		l ^= 0x80;
		r ^= 0x80;
	}
	for (c->c_acc += out; c->c_acc >= in; c->c_acc -= in)
		emit(c, l, r);
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
	unsigned long most = ((unsigned long)ASC_RATE >> 8) / (c->c_fmt.f_rate >> 8) + 1;
	unsigned char b;

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
			if (c->c_blen == 0 && c->c_n + most > RING)
				return;
			b = c->c_in[c->c_inoff++];
			c->c_left--;
			c->c_body[c->c_blen++] = b;
			if (c->c_blen == fsize(c)) {
				frame(c, c->c_body);
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
	n = usacc / FRAMEUS;
	usacc %= FRAMEUS;
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
	memset((char *)c, 0, sizeof *c);
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

static void
setup(afdp, lfdp)
int *afdp, *lfdp;
{
	int p[2], fd;

	if ((*afdp = open("/dev/asc", O_RDWR)) < 0) {
		perror("sndd: /dev/asc");
		exit(1);
	}
	asc = (volatile unsigned char *)mmap((caddr_t)0, ASC_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, *afdp, (off_t)0);
	if ((caddr_t)asc == (caddr_t)-1) {
		perror("sndd: mmap");
		exit(1);
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
	int afd, lfd, i, k, n, fg = 0, tmo;
	register struct cl *c;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-f") == 0)
			fg = 1;
		else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc)
			vol = atoi(argv[++i]) & 7;
	setup(&afd, &lfd);
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
			if (e.se_front != front || e.se_fuid != fuid) {
				if (playing)
					stop();
				cur = 0;
				front = e.se_front;
				fuid = e.se_fuid;
			} else
				chipirq(e.se_irq);
		}
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
