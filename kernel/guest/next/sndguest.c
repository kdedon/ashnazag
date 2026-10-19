/*
 * Sound out for the audio server that kern_loader loads into the kernel.
 * The server's code is found by its shape when the kernel first calls one
 * of its procedures.  Its calls to the kernel's DMA start and abort come
 * here, and the buffers on its output channel go to the host (struct
 * hf_snd).  Buffers the host has played are marked done and the channel's
 * handler is scheduled, as the DMA interrupt would.  Its routine that
 * waits for the DMA to run returns at once; the one that sends the sound
 * command records the rate.
 *
 * The kernel's DMA channel: queue head at 0, handler 16, its argument 20,
 * its level 24.  A descriptor: next at 0, start 4, end 8, state 16 (0 until
 * done).
 */
#include "hfswire.h"

#define S ((struct hf_snd *)HF_SND)
#define W16(p, o) (*(unsigned short *)((p) + (o)))
#define W32(p, o) (*(unsigned long *)((p) + (o)))
#define DMA_READ 0x40000UL		/* the start flag of an input channel */
#define DONE 0x08000000UL

static int
spl6()
{
	int s;

	__asm__ __volatile__("movew %%sr,%0\n\tmovew %1,%%sr" : "=&d" (s) : "d" (0x2600));
	return s;
}

static void
splx(s)
	int s;
{
	__asm__ __volatile__("movew %0,%%sr" : : "d" (s));
}

/* what is queued is dropped */
static void
flush()
{
	S->done = S->head;
	S->from = S->head;
	S->gen++;
}

/* the channel's descriptors from d on go to the host */
static void
queue(d)
	unsigned long d;
{
	for (; d && S->head - S->done < HF_SNDBUFS; d = W32(d, 0)) {
		S->buf[S->head % HF_SNDBUFS][0] = W32(d, 4);
		S->buf[S->head % HF_SNDBUFS][1] = W32(d, 8);
		S->sub[S->head % HF_SNDBUFS] = d;
		S->head++;
	}
}

static void poll();

static void
schedule()
{
	if (!S->polling) {
		S->polling = 1;
		S->timeout(poll, 0, 1);
	}
}

/*
 * Played buffers are done, new ones on the channel go to the host.  A
 * restart after an abort, from the first buffer still playing, keeps them.
 */
static void
poll()
{
	unsigned long c = S->chan, d;
	long n, k;
	int s = spl6(), done = 0;

	S->polling = 0;
	if (S->abort) {
		flush();
		S->abort = 0;
		S->chan = c = 0;
	}
	while (S->done != S->head && (long)(S->tail - S->done) > 0) {
		W32(S->sub[S->done % HF_SNDBUFS], 16) = DONE;
		S->done++;
		done = 1;
	}
	if (c) {
		/* past those done and those queued */
		for (d = W32(c, 0); d && W32(d, 16); d = W32(d, 0))
			;
		for (n = S->head - S->done, k = 0; d && k < n; k++)
			d = W32(d, 0);
		queue(d);
	}
	splx(s);
	if (done && c)
		S->softint(W32(c, 24), W32(c, 16), W32(c, 20));
	if (c && S->head != S->done)
		schedule();
}

static void
snd_start(c, d, flags)
	unsigned long c, d, flags;
{
	int s;

	if (flags & DMA_READ)
		return;
	s = spl6();
	if (!(S->abort && S->chan == c && S->head != S->done && S->sub[S->done % HF_SNDBUFS] == d)) {
		flush();
		queue(d);
	}
	S->abort = 0;
	S->chan = c;
	splx(s);
	schedule();
}

static void
snd_abort(c)
	unsigned long c;
{
	if (c == S->chan)
		S->abort = 1;
	schedule();
}

/* the sound command: bit 4 halves the rate */
static void
snd_cmd(cmd)
	unsigned long cmd;
{
	if ((cmd & 7) == 7)
		S->rate = cmd & 0x10 ? 22050 : 44100;
}

/* bsr.l or jsr to "from" at p becomes one to "to" */
static void
retarget(p, from, to)
	unsigned short *p;
	unsigned long from, to;
{
	unsigned long a = (unsigned long)(p + 1);

	if (p[0] == 0x61ff && a + (p[1] << 16 | p[2]) == from) {
		p[1] = (to - a) >> 16;
		p[2] = to - a;
	} else if (p[0] == 0x4eb9 && (p[1] << 16 | p[2]) == from) {
		p[1] = to >> 16;
		p[2] = to;
	}
}

static void
jump(p, to)
	unsigned short *p;
	unsigned long to;
{
	p[0] = 0x4ef9;
	p[1] = to >> 16;
	p[2] = to;
}

/* the routine's entry (pea fp@; movea.l sp,fp) at or before p, within n words */
static unsigned short *
entry(p, n)
	unsigned short *p;
	int n;
{
	for (; n-- > 0; p--)
		if (p[0] == 0x4856 && p[1] == 0x2c4f)
			return p;
	return 0;
}

/*
 * The audio server around a: its wait for the DMA reads the channel's
 * register page (movea.l 28(An),a0 ... move.l 0x4000(a0),d0); its sound
 * command waits on bit 4 of the register byte at +2 with interrupts off.
 */
static void
find(a)
	unsigned long a;
{
	unsigned long e, lo, hi;
	unsigned short *p, *w = 0, *cmd = 0;
	int i;

	if (!S->lookup(*(unsigned long *)S->kmap, a, &e))
		return;
	lo = W32(e, 8);
	hi = W32(e, 12);
	if (hi - lo > 0x100000 || lo == S->other || (lo == S->server && W16(S->wait, 0) == 0x4e75))
		return;
	for (p = (unsigned short *)lo + 8; p + 16 < (unsigned short *)hi; p++) {
		if (!w && p[0] == 0x2028 && p[1] == 0x4000)
			for (i = 1; i < 8; i++)
				if ((p[-i] & 0xfff8) == 0x2068 && p[1 - i] == 28)
					w = entry(p - i, 24);
		if (!cmd && p[0] == 0x40c0 && p[1] == 0x46fc && p[2] == 0x2600 && p[9] == 0x0002 &&
		    p[10] == 0x0800 && p[11] == 0x0004)
			cmd = entry(p, 4);
	}
	if (!w || !cmd) {
		S->other = lo;
		return;
	}
	for (p = (unsigned short *)lo; p + 3 < (unsigned short *)hi; p++) {
		retarget(p, S->dmastart, (unsigned long)snd_start);
		retarget(p, S->dmaabort, (unsigned long)snd_abort);
	}
	jump(cmd, (unsigned long)snd_cmd);
	w[0] = 0x4e75;
	__asm__ __volatile__(".short 0xf4f8");		/* cpusha bc: the patched code reaches the instruction cache */
	S->server = lo;
	S->wait = (unsigned long)w;
	S->rate = 44100;
}

/* the kernel's call of a loaded server's procedure */
static long
snd_callproc(server, proc, arg)
	unsigned long server;
	long (*proc)();
	unsigned long arg;
{
	if (!proc)
		return 100;
	find((unsigned long)proc);
	proc(arg);
	return 0;
}

void
snd_init()
{
	S->callproc = (unsigned long)snd_callproc;
}
