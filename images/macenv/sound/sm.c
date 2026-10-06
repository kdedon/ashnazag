/*
 * sm.c -- SoundOut: without the Sound Manager 3.x, its sampled-sound
 * calls, played through the host's sound service.
 *
 * Commands run when issued: bufferCmd writes the samples to the
 * session's stream (blocking while the service's buffer is full),
 * callBackCmd and a synchronous SndPlay wait until they were heard.
 * Synthesizers, modifiers and compressed sound are not offered.
 */
#include "../../../kernel/mac/sound/sndio.h"

#define nullCmd		0
#define quietCmd	3
#define flushCmd	4
#define callBackCmd	13
#define soundCmd	80
#define bufferCmd	81
#define dataFlag	0x8000

#define stdSH		0x00
#define extSH		0xFF

#define notEnoughHardwareErr	(-201)
#define badFormat		(-206)
#define noErr			0

#define CHANSIZE	1060	/* SndChannel with the standard 128-command queue */
#define NOWN		16

extern long auxsys();
extern void settrap(), disposeptr(), callback(), hsetstate();
extern char *newptr();
extern long hlock();
extern void p_docommand(), p_doimmediate(), p_play(), p_newchannel(),
	p_disposechannel(), p_addmodifier(), p_control(), p_dispatch();

struct cmd {
	unsigned short	cmd;
	short		param1;
	long		param2;
};

extern void trace();

static int fd = -1;
static int incb;		/* inside a completion routine */
#define NPEND	16
static struct {			/* commands issued by a completion routine */
	char		*chan;
	struct cmd	c;
} pend[NPEND];
static int pn;
static char *own[NOWN];		/* channels this code allocated */
static struct sndfmt fmt;

#define G16(p)	(*(unsigned short *)(p))
#define G32(p)	(*(unsigned long *)(p))

#define I_NREAD		0x5301L

/* the service's stream, (re)opened when needed; a plain file left at its path is refused */
static int
attach()
{
	long n;

	if (fd >= 0)
		return 0;
	/* opened without waiting, then writes block (F_SETFL) */
	if ((fd = auxsys(5L, (long)SNDPATH, 6L, 0L)) < 0)
		return -1;
	trace('1');
	if (auxsys(54L, (long)fd, I_NREAD, (long)&n) < 0 || auxsys(62L, (long)fd, 4L, 2L) < 0) {
		auxsys(6L, (long)fd, 0L, 0L);
		fd = -1;
		return -1;
	}
	trace('2');
	return 0;
}

static int
put(cmd, p, n)
unsigned long cmd;
char *p;
unsigned long n;
{
	struct sndrec r;

	if (attach() < 0)
		return -1;
	r.r_cmd = cmd;
	r.r_len = n;
	if (auxsys(4L, (long)fd, (long)&r, (long)sizeof r) != sizeof r ||
	    (n && auxsys(4L, (long)fd, (long)p, (long)n) != (long)n)) {
		auxsys(6L, (long)fd, 0L, 0L);	/* the service went away */
		fd = -1;
		return -1;
	}
	return 0;
}

/* everything written so far has been heard */
static void
drain()
{
	struct sndrec r;
	long n, k;

	if (put(SNDR_DRAIN, (char *)0, 0L) < 0)
		return;
	for (n = 0; n < (long)sizeof r; n += k)
		if ((k = auxsys(3L, (long)fd, (long)&r + n, (long)sizeof r - n)) <= 0) {
			auxsys(6L, (long)fd, 0L, 0L);
			fd = -1;
			return;
		}
}

/* a sampled sound header: format, then the samples */
static int
play(h)
unsigned char *h;
{
	unsigned char *p = (unsigned char *)G32(h);
	unsigned long n, k;
	int enc = h[20];

	fmt.f_rate = G32(h + 8);
	if (enc == stdSH) {
		n = G32(h + 4);
		fmt.f_enc = SNDE_U8;
		fmt.f_chans = 1;
		if (!p)
			p = h + 22;
	} else if (enc == extSH) {
		fmt.f_chans = G32(h + 4);
		fmt.f_enc = G16(h + 48) == 16 ? SNDE_S16 : SNDE_U8;
		n = G32(h + 22) * fmt.f_chans * (fmt.f_enc == SNDE_S16 ? 2 : 1);
		if (!p)
			p = h + 64;
	} else
		return badFormat;
	if (fmt.f_chans < 1 || fmt.f_chans > 2)
		return badFormat;
	if (fmt.f_rate < (4000L << 16))
		fmt.f_rate = SND_DEFRATE;
	if (put(SNDR_FMT, (char *)&fmt, (unsigned long)sizeof fmt) < 0)
		return notEnoughHardwareErr;
	for (; n; n -= k, p += k) {
		k = n > SNDR_MAX ? SNDR_MAX : n;
		if (put(SNDR_PCM, (char *)p, k) < 0)
			return notEnoughHardwareErr;
	}
	return noErr;
}

static int
docmd(chan, c)
char *chan;
struct cmd *c;
{
	long cb;

	switch (c->cmd & ~dataFlag) {
	case bufferCmd:
		return play((unsigned char *)c->param2);
	case quietCmd:
	case flushCmd:
		(void)put(SNDR_FLUSH, (char *)0, 0L);
		return noErr;
	case callBackCmd:
		drain();
		if (chan && (cb = G32(chan + 8)) != 0) {
			incb++;
			callback(cb, chan, c);
			incb--;
		}
		return noErr;
	}
	return noErr;
}

/*
 * Commands run when issued, so a completion routine that queues the next
 * sound would recurse once per sound.  Its commands wait until it returns.
 */
static void
runq()
{
	struct cmd c;
	char *chan;
	int i;

	while (pn > 0) {
		chan = pend[0].chan;
		c = pend[0].c;
		for (i = 1; i < pn; i++)
			pend[i - 1] = pend[i];
		pn--;
		(void)docmd(chan, &c);
	}
}

int
sm_docommand(chan, c)
char *chan;
struct cmd *c;
{
	int e;

	if (incb) {
		if (pn == NPEND)
			return -203;		/* queueFull */
		pend[pn].chan = chan;
		pend[pn++].c = *c;
		return noErr;
	}
	e = docmd(chan, c);
	runq();
	return e;
}

int
sm_play(chan, h, async)
char *chan;
unsigned char **h;
long async;
{
	unsigned char *p, *q;
	struct cmd c;
	int n, e;
	long st;

	if (h == 0 || *h == 0)
		return badFormat;
	st = hlock(h);			/* the header is read across completion routines */
	p = *h;
	e = badFormat;
	if (G16(p) == 1)
		q = p + 4 + 6 * G16(p + 2);
	else if (G16(p) == 2)
		q = p + 4;
	else
		goto out;
	e = noErr;
	for (n = G16(q), q += 2; n > 0 && e == noErr; n--, q += 8) {
		c.cmd = G16(q);
		c.param1 = G16(q + 2);
		c.param2 = G32(q + 4);
		if (c.cmd & dataFlag)
			c.param2 += (long)p;
		if ((c.cmd & ~dataFlag) == soundCmd)
			c.cmd = bufferCmd;
		e = docmd(chan, &c);
	}
	runq();
	if (e == noErr && !async)
		drain();
out:
	hsetstate(h, st);
	return e;
}

int
sm_newchannel(chanp, user)
char **chanp;
long user;
{
	int i;

	if (*chanp == 0) {
		for (i = 0; i < NOWN && own[i]; i++)
			;
		if (i == NOWN || (*chanp = newptr((long)CHANSIZE)) == 0)
			return notEnoughHardwareErr;
		own[i] = *chanp;
		*(short *)(*chanp + 30) = 128;	/* qLength */
	}
	G32(*chanp + 8) = user;
	return noErr;
}

int
sm_disposechannel(chan, quiet)
char *chan;
long quiet;
{
	int i;

	if (quiet)
		(void)put(SNDR_FLUSH, (char *)0, 0L);
	else
		drain();
	for (i = 0; i < NOWN; i++)
		if (own[i] && own[i] == chan) {
			own[i] = 0;
			disposeptr(chan);
		}
	return noErr;
}

extern int sdev_install();
extern void sdev_test();

/*
 * From the INIT, in the system-heap copy: the Sound Manager 3.x plays
 * through the output component; without it these calls replace its traps.
 */
void
sm_install()
{
	long old;

	/* a write to a service that went away must not end the session */
	if ((old = auxsys(48L, 13L, 1L, 0L)) != 0 && old != 1 && old != -1)
		(void)auxsys(48L, 13L, old, 0L);
	trace('i');
	if (sdev_install()) {
		sdev_test(1);
		return;
	}
	/* the stream is opened at the first sound */
	trace('a');
	if (auxsys(33L, (long)SNDPATH, 0L, 0L) < 0)
		return;
	trace('b');
	settrap(0xA800L, p_dispatch);
	settrap(0xA801L, p_disposechannel);
	settrap(0xA802L, p_addmodifier);
	settrap(0xA803L, p_docommand);
	settrap(0xA804L, p_doimmediate);
	settrap(0xA805L, p_play);
	settrap(0xA806L, p_control);
	settrap(0xA807L, p_newchannel);
	trace('t');
	sdev_test(0);
}
