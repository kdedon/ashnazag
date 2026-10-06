/*
 * SndTest: with /tmp/sndtest present at startup, plays through the Mac's
 * own Sound Manager and writes /tmp/sndtest.out:
 *
 *   sync_err=<e> sync_ticks=<t> err=<e> async_ticks=<t> cb=<0|1> cb_ticks=<t>
 *
 * SysBeep, a synchronous SndPlay of half a second with no channel, then an
 * asynchronous SndPlay of one second on a channel and a callBackCmd, whose
 * routine writes the line's end.  Before each, a byte is appended to
 * /tmp/sndtest.trace and the test is given half a second to count the
 * chip's frames so far.
 */
#define TESTF	"/tmp/sndtest"
#define CHIPHZ	22254
#define CHANSIZE 1060		/* SndChannel with a 128-command queue */
#define ASC_RATE 0x56EE8BA3

#define G16(p)	(*(unsigned short *)(p))
#define G32(p)	(*(unsigned long *)(p))

extern long auxsysr(), sndnew(), sndplay(), snddo();
extern char **newhandlesys();
extern char *newptr();
extern unsigned long ticks();
extern void sysbeep(), t_cb();

static unsigned long t0;

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

/* at least n ticks by the Unix clock: System 6 stops Ticks while INITs run */
static void
wait(n)
unsigned long n;
{
	long t = auxsysr(13L, 0L, 0L, 0L);

	while (auxsysr(13L, 0L, 0L, 0L) - t < (long)(n + 59) / 60 + 1)
		;
}

static void
mark()
{
	long fd;

	if ((fd = auxsysr(5L, (long)TESTF ".trace", 0x109L, 0666L)) >= 0) {
		auxsysr(4L, fd, (long)"m", 1L);
		auxsysr(6L, fd, 0L, 0L);
	}
	wait(30L);
}

/* format 2 'snd ': one bufferCmd, its standard header at 14, samples at 36 */
static char **
sound(n, hz)
long n, hz;
{
	char **h, *p;
	long i;

	if ((h = newhandlesys(36L + n)) == 0)
		return 0;
	p = *h;
	G16(p) = 2;
	G16(p + 4) = 1;
	G16(p + 6) = 0x8051;
	G32(p + 10) = 14;
	G32(p + 18) = n;
	G32(p + 22) = ASC_RATE;
	p[35] = 60;
	for (i = 0; i < n; i++)
		p[36 + i] = (i * hz / CHIPHZ) & 1 ? 0xA0 : 0x60;
	return h;
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
	q[-1] = '\n';
	out(line, q, 0);
}

void
st_main()
{
	struct { unsigned short cmd; short p1; long p2; } c;
	unsigned long t1;
	char **h1, **h2, *chan, line[160], *q = line;
	long e;

	if (auxsysr(33L, (long)TESTF, 0L, 0L) < 0)
		return;
	h1 = sound((long)CHIPHZ / 2, 800L);
	h2 = sound((long)CHIPHZ, 1200L);
	if (h1 == 0 || h2 == 0 || (chan = newptr((long)CHANSIZE)) == 0)
		return;
	*(short *)(chan + 30) = 128;	/* qLength */

	mark();
	sysbeep(30L);
	wait(90L);
	mark();
	t0 = ticks();
	e = sndplay(0L, h1, 0L);
	t1 = ticks();
	wait(60L);
	mark();
	q = put(q, "sync_err=", (unsigned long)-e);
	q = put(q, "sync_ticks=", t1 - t0);

	e = sndnew(&chan, 5L, 0L, t_cb);	/* sampledSynth */
	t0 = ticks();
	if (e == 0)
		e = sndplay(chan, h2, 1L);
	t1 = ticks();
	q = put(q, "err=", (unsigned long)-e);
	q = put(q, "async_ticks=", t1 - t0);
	out(line, q, 1);
	c.cmd = 13;			/* callBackCmd */
	c.p1 = 0;
	c.p2 = 0x5A5AL;
	if (e != 0 || snddo(chan, &c) != 0) {
		q = line;
		q = put(q, "cb=", 0L);
		q = put(q, "cb_ticks=", 0L);
		q[-1] = '\n';
		out(line, q, 0);
	}
}
