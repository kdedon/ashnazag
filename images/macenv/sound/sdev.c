/*
 * sdev.c -- SoundOut's Sound Manager 3.x output component.
 *
 * The Sound Manager keeps channels, queues, synthesizers and its mixer;
 * this component only takes the mixer's output.  A Time Manager task
 * pulls mixed buffers at the playback rate into a ring; a deferred task
 * writes the ring to the session's stream, never blocking:
 *
 *   mixer --GetSourceData--> tm_run --ring--> dt_run --write--> sndd
 *
 * Both run as interrupts, which here are Unix signals, so a write that
 * cannot block is allowed; a full stream waits for the next tick.
 */
#include "../../../kernel/mac/sound/sndio.h"

#define OSTYPE(a, b, c, d)	((long)(a) << 24 | (long)(b) << 16 | (c) << 8 | (d))

#define noErr			0
#define siUnknownInfoType	(-231)
#define badComponentSelector	0x80008002L

#define NFR	512		/* frames per mixer buffer */
#define LEAD	3072		/* frames pulled ahead of the clock, 0.14 s */
#define RB	16384		/* ring bytes: 16-bit stereo frames */
#define CHUNK	4096		/* most PCM bytes per record */
#define PERIOD	10		/* ms between pulls */
#define VOL1	0x0100		/* unity in 'hvol' */

extern unsigned long ticks();
extern long auxsysr(), splhi(), gettrap(), smversion(), newhandlesys();
extern long delegate(), registercomp(), findnext(), capture(), setdefault();
extern long getsrc(), openmixer(), closemixer();
extern void splx(), instime(), primetime(), dtinstall();
extern void p_sdev(), t_tm(), t_dt();

#define G16(p)	(*(unsigned short *)(p))
#define G32(p)	(*(unsigned long *)(p))

#define I_NREAD	0x5301L
#define EAGAIN	11
#define EINTR	4

static struct {			/* SoundComponentData */
	long	flags;
	long	format;
	short	chans;
	short	size;
	long	rate;
	long	count;
	char	*buf;
	long	res;
} outdesc;

static struct {			/* TMTask */
	long	link;
	short	type;
	long	addr;
	long	count, wake, res;
} tm;

static struct {			/* DeferredTask */
	long	link;
	short	type;
	short	flags;
	long	addr;
	long	param, res;
} dt;

static long mixer;		/* the Apple Mixer's instance */
static long me;			/* this component */
static long hvol = (long)VOL1 << 16 | VOL1;
static short mute;
static volatile short running;	/* the mixer has sources */
static volatile short tmon;	/* the Time Manager task is primed */
static volatile short dtq;	/* the deferred task is queued or running */
static unsigned long last;	/* Ticks at the last pull */
static long ahead;		/* frames pulled, not yet due */
unsigned long sdev_frames;	/* frames taken from the mixer */

static unsigned char ring[RB];
static volatile long head, tail;

static int sfd = -1;
static unsigned long retry;	/* Ticks at the last failed open */
static short needfmt;
static unsigned char hdr[16];	/* the record being written: header (+ format) */
static long hoff, hlen, left;	/* its header progress, PCM bytes still due */

void
trace(c)
int c;
{
	char b = c;
	long fd;

	if (auxsysr(33L, (long)"/tmp/sndtest", 0L, 0L) >= 0 &&
	    (fd = auxsysr(5L, (long)"/tmp/sndtest.trace", 0x109L, 0666L)) >= 0) {
		auxsysr(4L, fd, (long)&b, 1L);
		auxsysr(6L, fd, 0L, 0L);
	}
}

/* the stream, opened without blocking; a plain file at its path is refused */
static int
attach()
{
	long n;

	if (sfd >= 0)
		return 0;
	if (ticks() - retry < 60)
		return -1;
	retry = ticks();
	if ((sfd = auxsysr(5L, (long)SNDPATH, 6L, 0L)) < 0) {	/* O_RDWR|O_NDELAY */
		sfd = -1;
		return -1;
	}
	trace('3');
	if (auxsysr(54L, (long)sfd, I_NREAD, (long)&n) < 0) {
		auxsysr(6L, (long)sfd, 0L, 0L);
		sfd = -1;
		return -1;
	}
	trace('4');
	needfmt = 1;
	hoff = hlen = left = 0;
	return 0;
}

static void
detach()
{
	auxsysr(6L, (long)sfd, 0L, 0L);
	sfd = -1;
	retry = ticks();
	tail = head;
	hoff = hlen = left = 0;
}

/* bytes written, 0 when the stream is full, -1 when it is gone */
static long
wr(p, n)
unsigned char *p;
long n;
{
	long k = auxsysr(4L, (long)sfd, (long)p, n);

	if (k > 0)
		return k;
	if (k == 0 || k == -EAGAIN || k == -EINTR)
		return 0;
	detach();
	return -1;
}

static void
sethdr(cmd, len)
unsigned long cmd, len;
{
	struct sndrec *r = (struct sndrec *)hdr;

	r->r_cmd = cmd;
	r->r_len = len;
	hoff = 0;
	hlen = sizeof *r;
}

/* the ring to the stream as PCM records, resuming a partial write */
static void
flushout()
{
	struct sndfmt *f = (struct sndfmt *)(hdr + 8);
	long n, k;

	if (attach() < 0) {
		tail = head;		/* no service: dropped, the mixer keeps time */
		return;
	}
	for (;;) {
		if (hoff < hlen) {
			if ((k = wr(hdr + hoff, hlen - hoff)) <= 0)
				return;
			hoff += k;
			continue;
		}
		if (left > 0) {
			n = RB - tail;
			if (n > left)
				n = left;
			if ((k = wr(ring + tail, n)) <= 0)
				return;
			tail = (tail + k) & (RB - 1);
			left -= k;
			continue;
		}
		if (needfmt) {
			sethdr((unsigned long)SNDR_FMT, (unsigned long)sizeof *f);
			f->f_rate = ASC_RATE;
			f->f_enc = SNDE_S16;
			f->f_chans = 2;
			hlen += sizeof *f;
			needfmt = 0;
			continue;
		}
		if ((n = (head - tail) & (RB - 1)) == 0)
			return;
		left = n > CHUNK ? CHUNK : n;
		sethdr((unsigned long)SNDR_PCM, (unsigned long)left);
	}
}

void
dt_run()
{
	flushout();
	dtq = 0;
}

static long
scale(s, v)
long s, v;
{
	return mute ? 0 : v == VOL1 ? s : s * v >> 8;
}

/*
 * One mixer buffer into the ring as 16-bit stereo; 0 when the mixer is
 * idle.  Silence keeps time but is not sent, so the chip can stop.
 */
static int
pull()
{
	char *d = 0;
	unsigned char *p;
	short *o;
	long n, i, l, r, w, vl, vr, nz = 0;
	int ch, sz;

	if (getsrc(mixer, &d) != noErr || d == 0)
		return 0;
	n = G32(d + 16);
	p = (unsigned char *)G32(d + 20);
	ch = G16(d + 8);
	sz = G16(d + 10);
	if (n <= 0 || p == 0)
		return 0;
	if (n > NFR)
		n = NFR;
	vl = (unsigned long)hvol >> 16;
	vr = hvol & 0xffff;
	w = head;
	for (i = 0; i < n; i++) {
		if (sz == 16) {
			l = ((short *)p)[0];
			r = ch == 2 ? ((short *)p)[1] : l;
			p += ch * 2;
		} else {
			l = G32(d + 4) == OSTYPE('r','a','w',' ') ? (long)p[0] - 128 : (long)(signed char)p[0];
			r = ch == 2 ? (G32(d + 4) == OSTYPE('r','a','w',' ') ? (long)p[1] - 128 : (long)(signed char)p[1]) : l;
			l <<= 8;
			r <<= 8;
			p += ch;
		}
		l = scale(l, vl);
		r = scale(r, vr);
		o = (short *)(ring + w);
		o[0] = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
		o[1] = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
		nz |= l | r;
		w = (w + 4) & (RB - 1);
	}
	if (nz)
		head = w;
	ahead += n;
	sdev_frames += n;
	return 1;
}

/*
 * The Time Manager task: frames come due at the chip's rate, about 371 a
 * tick; keep LEAD of them pulled while the ring has room, hand them to the
 * deferred writer, and rearm while there is anything to do.
 */
void
tm_run()
{
	unsigned long now = ticks(), el;
	long due, used;

	el = now - last;
	last = now;
	if (el > 60)
		el = 60;
	due = (long)el * 371;
	ahead = ahead > due ? ahead - due : 0;
	while (running && ahead < LEAD && RB - 4 - ((head - tail) & (RB - 1)) >= NFR * 4)
		if (!pull())
			running = 0;
	used = (head - tail) & (RB - 1);
	if ((used || hoff < hlen || needfmt) && !dtq) {
		dtq = 1;
		dtinstall(&dt);
	}
	if (running || used)
		primetime(&tm, (long)PERIOD);
	else
		tmon = 0;
}

/* a source started: run the pulls if they stopped */
static void
kick()
{
	long s = splhi();

	running = 1;
	if (!tmon) {
		tmon = 1;
		last = ticks();
		ahead = 0;
		primetime(&tm, 1L);
	}
	splx(s);
}

/* GetInfo for what the hardware decides; the rest is the mixer's */
static long
getinfo(pa, sel, p)
char *pa;
long sel;
char *p;
{
	char **h;

	switch (sel) {
	case OSTYPE('s','r','a','t'):
		G32(p) = ASC_RATE;
		return noErr;
	case OSTYPE('s','s','i','z'):
		G16(p) = 16;
		return noErr;
	case OSTYPE('c','h','a','n'):
	case OSTYPE('c','h','a','v'):
		G16(p) = 2;
		return noErr;
	case OSTYPE('h','v','o','l'):
		G32(p) = hvol;
		return noErr;
	case OSTYPE('h','m','u','t'):
		G16(p) = mute;
		return noErr;
	case OSTYPE('h','w','b','s'):
		G16(p) = running || ((head - tail) & (RB - 1)) != 0;
		return noErr;
	case OSTYPE('s','r','a','v'):
	case OSTYPE('s','s','a','v'):
		/* SoundInfoList: one entry, a handle the caller disposes */
		if ((h = (char **)newhandlesys(4L)) == 0)
			return -108;
		if (sel == OSTYPE('s','r','a','v'))
			G32(*h) = ASC_RATE;
		else
			G16(*h) = 16;
		G16(p) = 1;
		G32(p + 2) = (long)h;
		return noErr;
	}
	return mixer ? delegate(pa, mixer) : siUnknownInfoType;
}

static long
setinfo(pa, sel, v)
char *pa;
long sel, v;
{
	switch (sel) {
	case OSTYPE('h','v','o','l'):
		hvol = v;
		if ((unsigned long)hvol >> 16 > VOL1)
			hvol = (long)VOL1 << 16 | (hvol & 0xffff);
		if ((hvol & 0xffff) > VOL1)
			hvol = (hvol & 0xffff0000) | VOL1;
		return noErr;
	case OSTYPE('h','m','u','t'):
		mute = v != 0;
		return noErr;
	case OSTYPE('s','r','a','t'):
		return v == ASC_RATE ? noErr : -232;	/* siInvalidSampleRate */
	case OSTYPE('s','s','i','z'):
		return v == 16 ? noErr : -233;		/* siInvalidSampleSize */
	}
	return mixer ? delegate(pa, mixer) : siUnknownInfoType;
}

/* the component's calls; params are the last argument first */
long
sdev(pa, storage)
char *pa;
long storage;
{
	char *pp = pa + 4;
	long e;
	int what = (short)G16(pa + 2);

	switch (what) {
	case -1:			/* open */
		return noErr;
	case -4:			/* version */
		return 0x00010000L;
	case -2:			/* close */
		running = 0;
		if (mixer)
			closemixer(mixer);
		mixer = 0;
		return noErr;
	case -3:			/* can do */
		what = (short)G16(pp);
		return (what >= -5 && what <= -1) || (what >= 1 && what <= 5) ||
		    (what >= 0x101 && what <= 0x108);
	case -5:			/* register */
		return noErr;
	case 1:				/* InitOutputDevice */
		trace('o');
		if (mixer)
			return noErr;
		e = openmixer(&outdesc, 0L, &mixer);
		trace('m');
		return e;
	case 0x103:			/* GetInfo(source, selector, infoPtr) */
		return getinfo(pa, (long)G32(pp + 4), (char *)G32(pp));
	case 0x104:			/* SetInfo */
		return setinfo(pa, (long)G32(pp + 4), (long)G32(pp));
	case 0x105:			/* StartSource */
	case 0x108:			/* PlaySourceBuffer */
		trace('s');
		if (!mixer)
			return badComponentSelector;
		if ((e = delegate(pa, mixer)) == noErr)
			kick();
		return e;
	default:
		return mixer ? delegate(pa, mixer) : badComponentSelector;
	}
}

/* 1 if the Sound Manager 3.x is there, and it now plays through here */
int
sdev_install()
{
	static long desc[5];
	char **name;
	long c, n;

	trace('g');
	if (gettrap(0xA800L) == gettrap(0xA89FL))
		return 0;
	trace('v');
	if ((unsigned long)smversion() >> 24 < 3)
		return 0;
	trace('f');
	desc[0] = OSTYPE('m','i','x','r');
	if (findnext(0L, desc) == 0)
		return 0;
	trace('h');
	outdesc.format = OSTYPE('t','w','o','s');
	outdesc.chans = 2;
	outdesc.size = 16;
	outdesc.rate = ASC_RATE;
	outdesc.count = NFR;
	tm.addr = (long)t_tm;
	dt.type = 7;			/* dtQType */
	dt.addr = (long)t_dt;
	if ((name = (char **)newhandlesys(16L)) != 0) {
		(*name)[0] = 10;
		for (n = 0; n < 10; n++)
			(*name)[n + 1] = "Unix sound"[n];
	}
	/* stereo 16-bit in, under the built-in device's name */
	desc[0] = OSTYPE('s','d','e','v');
	desc[1] = OSTYPE('a','s','c',' ');
	desc[2] = OSTYPE('a','p','p','l');
	desc[3] = 0x0000000C;
	desc[4] = 0;
	if ((me = registercomp(desc, p_sdev, name)) == 0)
		return 0;
	trace('r');
	/* the hardware's own output components are hidden behind this one */
	desc[1] = desc[2] = desc[3] = 0;
	for (c = findnext(0L, desc); c; c = n) {
		n = findnext(c, desc);
		if (c != me)
			capture(c, me);
	}
	setdefault(me, 7L);		/* any flags, manufacturer, subtype */
	instime(&tm);
	trace('d');
	return 1;
}

/*
 * Self-test, when TESTF exists at startup: an asynchronous SndPlay of a
 * one-second tone, then a callBackCmd.  The INIT writes the start of a
 * line to TESTF ".out" and returns; the callback finishes it.
 */
#define TESTF	"/tmp/sndtest"
#define TESTN	22254
#define CHANSIZE 1060		/* SndChannel with a 128-command queue */

extern long sndnew(), sndplay(), snddo();
extern char *newptr();
extern long hlock();
extern void t_cb();
static unsigned long t0, f0;

static char *
put(q, k, v)
char *q, *k;
unsigned long v;
{
	char b[12];
	int i = 0;

	while (*k)
		*q++ = *k++;
	do
		b[i++] = '0' + v % 10;
	while ((v /= 10) != 0);
	while (i > 0)
		*q++ = b[--i];
	*q++ = ' ';
	return q;
}

/* O_WRONLY|O_APPEND|O_CREAT, or with O_TRUNC */
static void
out(line, q, trunc)
char *line, *q;
int trunc;
{
	long fd;

	if ((fd = auxsysr(5L, (long)TESTF ".out", trunc ? 0x309L : 0x109L, 0666L)) >= 0) {
		auxsysr(4L, fd, (long)line, (long)(q - line));
		auxsysr(6L, fd, 0L, 0L);
	}
}

/* the callBackCmd, at interrupt time */
void
cb_run(p2)
long p2;
{
	char line[64], *q = line;

	if (p2 != 0x5A5AL)
		return;
	q = put(q, "cb=", 1L);
	q = put(q, "cb_ticks=", ticks() - t0);
	q = put(q, "frames=", sdev_frames - f0);
	q[-1] = '\n';
	out(line, q, 0);
}

void
sdev_test(comp)
int comp;
{
	struct { unsigned short cmd; short p1; long p2; } c;
	unsigned long t1;
	char **h, *p, *chan, line[128], *q;
	long e, i;

	if (auxsysr(33L, (long)TESTF, 0L, 0L) < 0)
		return;
	/* both in the system heap: the sound outlives this INIT */
	if ((h = (char **)newhandlesys(36L + TESTN)) == 0 || (chan = newptr((long)CHANSIZE)) == 0)
		return;
	*(short *)(chan + 30) = 128;	/* qLength */
	(void)hlock(h);
	/* format 2: one bufferCmd, its standard header at 14, samples at 36 */
	p = *h;
	G16(p) = 2;
	G16(p + 4) = 1;
	G16(p + 6) = 0x8051;
	G32(p + 10) = 14;
	G32(p + 18) = TESTN;
	G32(p + 22) = ASC_RATE;
	p[35] = 60;
	for (i = 0; i < TESTN; i++)
		p[36 + i] = (i / 50) & 1 ? 0xA0 : 0x60;
	f0 = sdev_frames;
	trace('n');
	e = sndnew(&chan, 5L, 0L, t_cb);	/* sampledSynth */
	trace('p');
	t0 = ticks();
	if (e == noErr)
		e = sndplay(chan, h, 1L);
	t1 = ticks();
	trace('q');
	q = line;
	for (p = comp ? "path=component " : "path=traps "; *p; )
		*q++ = *p++;
	q = put(q, "err=", (unsigned long)-e);
	q = put(q, "async_ticks=", t1 - t0);
	out(line, q, 1);
	c.cmd = 13;			/* callBackCmd */
	c.p1 = 0;
	c.p2 = 0x5A5AL;
	if (e != noErr || snddo(chan, &c) != noErr) {
		q = line;
		q = put(q, "cb=", 0L);
		q = put(q, "cb_ticks=", 0L);
		q = put(q, "frames=", 0L);
		q[-1] = '\n';
		out(line, q, 0);
	}
}
