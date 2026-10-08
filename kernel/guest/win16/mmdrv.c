/*
 * mmdrv.c -- the multimedia drivers that are ours, as Wabi's were: the
 * timer (TIMER.DRV, MMSYSTEM's clock and timeSetEvent) and the wave
 * output (ASHAUDIO.DRV) on the session's sound (snd.h).  MMSYSTEM is the
 * user's; it opens these through SYSTEM.INI's [drivers] (timer=, wave=)
 * and calls their DriverProc and wodMessage.
 *
 * Windows runs a driver's work at interrupt time; here mm_tick() does
 * it, from the message loop and between API calls: samples fed a lead
 * ahead of the clock, buffers marked done when they have played and the
 * program told through MMSYSTEM's DriverCallback, timer events called.
 * Recording, MIDI and auxiliary devices: none (no devices reported).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "win.h"
#include "snd.h"

#define	DRV_LOAD	1
#define	DRV_ENABLE	2
#define	DRV_OPEN	3
#define	DRV_CLOSE	4
#define	DRV_DISABLE	5
#define	DRV_FREE	6
#define	DRV_QUERYCONFIGURE 8
#define	DRV_INSTALL	9
#define	DRV_REMOVE	10

#define	TDD_KILLTIMEREVENT	0x800
#define	TDD_SETTIMEREVENT	0x804
#define	TDD_GETSYSTEMTIME	0x808
#define	TDD_GETDEVCAPS		0x80c
#define	TDD_BEGINMINPERIOD	0x810
#define	TDD_ENDMINPERIOD	0x814

#define	WODM_GETNUMDEVS	3
#define	WODM_GETDEVCAPS	4
#define	WODM_OPEN	5
#define	WODM_CLOSE	6
#define	WODM_PREPARE	7
#define	WODM_UNPREPARE	8
#define	WODM_WRITE	9
#define	WODM_PAUSE	10
#define	WODM_RESTART	11
#define	WODM_RESET	12
#define	WODM_GETPOS	13
#define	WODM_BREAKLOOP	20

#define	WOM_OPEN	0x3bb
#define	WOM_CLOSE	0x3bc
#define	WOM_DONE	0x3bd

#define	WHDR_DONE	0x01
#define	WHDR_INQUEUE	0x10

#define	MMSYSERR_BADDEVICEID	2
#define	MMSYSERR_ALLOCATED	4
#define	MMSYSERR_NOTSUPPORTED	8
#define	MMSYSERR_INVALPARAM	11
#define	WAVERR_BADFORMAT	32
#define	WAVERR_STILLPLAYING	33

#define	LEAD	400		/* ms of samples fed ahead */
#define	NTEV	16
#define	NHDR	64

/* ---- the timer ---- */

struct tev {
	int	used, periodic;
	u32	due, ms, fn, user;
};

static struct tev tevs[NTEV];

static u32
timerproc(a)
	u32 *a;
{
	u32 p;
	int i;

	switch (a[2] & 0xffff) {
	case DRV_LOAD: case DRV_ENABLE: case DRV_DISABLE: case DRV_FREE:
	case DRV_OPEN: case DRV_CLOSE: case DRV_INSTALL: case DRV_REMOVE:
		return 1;
	case TDD_GETSYSTEMTIME:
		return w16_ticks();
	case TDD_GETDEVCAPS:
		/* TIMECAPS: the shortest and longest periods, ms */
		if ((p = lin(FPSEL(a[3]), FPOFF(a[3]))) != 0 && a[4] >= 4) {
			PW(p, 1);
			PW(p + 2, 0xffff);
		}
		return 0;
	case TDD_SETTIMEREVENT:
		/* TIMEREVENT: delay, resolution, function, its dword, flags (TIME_PERIODIC 1) */
		if ((p = lin(FPSEL(a[3]), FPOFF(a[3]))) == 0)
			return 0;
		for (i = 0; i < NTEV && tevs[i].used; i++)
			;
		if (i == NTEV)
			return 0;
		tevs[i].used = 1;
		tevs[i].ms = GW(p) ? GW(p) : 1;
		tevs[i].fn = GL(p + 4);
		tevs[i].user = GL(p + 8);
		tevs[i].periodic = GW(p + 12) & 1;
		tevs[i].due = w16_ticks() + tevs[i].ms;
		return i + 1;
	case TDD_KILLTIMEREVENT:
		if (a[3] >= 1 && a[3] <= NTEV)
			tevs[a[3] - 1].used = 0;
		return 0;
	case TDD_BEGINMINPERIOD:
	case TDD_ENDMINPERIOD:
		return 0;
	}
	return 0;
}

/* ---- wave output ---- */

static struct {
	int	open, paused;
	u32	cb, inst, cbflags;	/* DriverCallback's: where, its dword, DCB_ type */
	u16	hwave;
	long	rate;
	int	chans, bits, align;
	u32	q[NHDR];		/* the buffers given, in order */
	unsigned long qend[NHDR];	/* the frame each ends at, once fed (0: not yet) */
	int	nq, fed, off;		/* buffers fully fed, bytes into the next */
	unsigned long base;		/* frame of position 0 */
} wo;

static int busy;		/* inside mm_tick */

/* MMSYSTEM's DriverCallback: the program told, as its open asked */
static void
callback(msg, p1)
	int msg;
	u32 p1;
{
	static u32 dcb;
	struct module *m;

	if (!wo.cbflags)
		return;
	if (!dcb && ((m = mod_find("MMSYSTEM")) == 0 || (dcb = mod_proc(m, 0, "DriverCallback")) == 0))
		return;
	cb_begin();
	cb_push32(wo.cb);
	cb_push16(wo.cbflags);
	cb_push16(wo.hwave);
	cb_push16(msg);
	cb_push32(wo.inst);
	cb_push32(p1);
	cb_push32((u32)0);
	cb_call(dcb, 0);
}

static void
done(i)
	int i;
{
	u32 h = lin(FPSEL(wo.q[i]), FPOFF(wo.q[i])), hdr = wo.q[i];

	memmove(&wo.q[i], &wo.q[i + 1], (wo.nq - i - 1) * sizeof wo.q[0]);
	memmove(&wo.qend[i], &wo.qend[i + 1], (wo.nq - i - 1) * sizeof wo.qend[0]);
	wo.nq--;
	if (wo.fed > i)
		wo.fed--;
	if (h)
		PL(h + 16, (GL(h + 16) & ~WHDR_INQUEUE) | WHDR_DONE);
	callback(WOM_DONE, hdr);
}

/* feed what is due; the buffers that have played are done */
static void
wavepump()
{
	long n, k, avail;
	u32 h, data, len;

	if (!wo.open)
		return;
	if (!wo.paused) {
		n = snd_due((long)LEAD);
		while (n > 0 && wo.fed < wo.nq) {
			h = lin(FPSEL(wo.q[wo.fed]), FPOFF(wo.q[wo.fed]));
			data = h ? lin(FPSEL(GL(h)), FPOFF(GL(h))) : 0;
			len = h ? GL(h + 4) : 0;
			avail = data && len > (u32)wo.off ? (long)(len - wo.off) / wo.align : 0;
			k = avail < n ? avail : n;
			if (k > 0) {
				snd_put((char *)M + data + wo.off, k);
				wo.off += k * wo.align;
				n -= k;
			}
			if (k == avail) {
				wo.qend[wo.fed] = snd_taken();
				wo.fed++;
				wo.off = 0;
			}
		}
	}
	while (wo.nq > 0 && wo.fed > 0 && (long)(snd_now() - wo.qend[0]) >= 0)
		done(0);
}

static int
fmtok(f)
	u32 f;
{
	int ch, bits;
	u32 rate;

	if (!f || GW(f) != 1)		/* WAVE_FORMAT_PCM */
		return 0;
	ch = GW(f + 2);
	rate = GL(f + 4);
	bits = GW(f + 14);
	return (ch == 1 || ch == 2) && (bits == 8 || bits == 16) && rate >= 4000 && rate <= 48000 &&
	    GW(f + 12) == ch * bits / 8;
}

static u32
wodmessage(a)
	u32 *a;
{
	u32 p, f;
	unsigned long t;

	if ((a[1] & 0xffff) != WODM_GETNUMDEVS && (a[0] & 0xffff) != 0)
		return MMSYSERR_BADDEVICEID;
	switch (a[1] & 0xffff) {
	case WODM_GETNUMDEVS:
		return 1;
	case WODM_GETDEVCAPS:
		/* WAVEOUTCAPS: maker, product, version, name[32], formats, channels, support */
		if ((p = lin(FPSEL(a[3]), FPOFF(a[3]))) != 0) {
			char caps[48];

			memset(caps, 0, sizeof caps);
			caps[4] = 0;
			caps[5] = 1;		/* version 1.0 */
			strcpy(caps + 6, "Ash Nazag Sound");
			caps[38] = (char)0xff;	/* every 11, 22, 44 kHz format */
			caps[39] = 0x0f;
			caps[42] = 2;
			memcpy(M + p, caps, a[4] < 48 ? a[4] : 48);
		}
		return 0;
	case WODM_OPEN:
		/* dwUser: where our instance goes; WAVEOPENDESC: hWave, lpFormat, dwCallback, dwInstance */
		if ((p = lin(FPSEL(a[3]), FPOFF(a[3]))) == 0)
			return MMSYSERR_INVALPARAM;
		f = lin(FPSEL(GL(p + 2)), FPOFF(GL(p + 2)));
		if (!fmtok(f))
			return WAVERR_BADFORMAT;
		if (a[4] & 1)		/* WAVE_FORMAT_QUERY */
			return 0;
		if (wo.open)
			return MMSYSERR_ALLOCATED;
		memset((char *)&wo, 0, sizeof wo);
		wo.open = 1;
		wo.hwave = GW(p);
		wo.cb = GL(p + 6);
		wo.inst = GL(p + 10);
		wo.cbflags = (a[4] >> 16) & 7;
		wo.chans = GW(f + 2);
		wo.rate = GL(f + 4);
		wo.bits = GW(f + 14);
		wo.align = wo.chans * wo.bits / 8;
		snd_start(wo.rate, wo.bits, wo.chans);
		wo.base = snd_now();
		if ((p = lin(FPSEL(a[2]), FPOFF(a[2]))) != 0)
			PL(p, 1);
		callback(WOM_OPEN, (u32)0);
		return 0;
	case WODM_CLOSE:
		if (!wo.open)
			return MMSYSERR_INVALPARAM;
		if (wo.nq)
			return WAVERR_STILLPLAYING;
		callback(WOM_CLOSE, (u32)0);
		wo.open = 0;
		return 0;
	case WODM_PREPARE:
	case WODM_UNPREPARE:
		return MMSYSERR_NOTSUPPORTED;	/* MMSYSTEM does it */
	case WODM_WRITE:
		if (!wo.open || (p = lin(FPSEL(a[3]), FPOFF(a[3]))) == 0)
			return MMSYSERR_INVALPARAM;
		if (wo.nq == NHDR)
			return MMSYSERR_NOTSUPPORTED;
		PL(p + 16, (GL(p + 16) & ~WHDR_DONE) | WHDR_INQUEUE);
		wo.q[wo.nq] = a[3];
		wo.qend[wo.nq] = 0;
		wo.nq++;
		if (!busy) {
			busy = 1;
			wavepump();
			busy = 0;
		}
		return 0;
	case WODM_PAUSE:
		wo.paused = 1;
		return 0;
	case WODM_RESTART:
		wo.paused = 0;
		return 0;
	case WODM_RESET:
		snd_flush();
		wo.paused = 0;
		wo.fed = wo.nq;
		while (wo.nq > 0)
			done(0);
		wo.off = 0;
		wo.base = snd_now();
		return 0;
	case WODM_GETPOS:
		/* MMTIME: wType, then the time in it; TIME_MS 1, TIME_SAMPLES 2, TIME_BYTES 4 */
		if ((p = lin(FPSEL(a[3]), FPOFF(a[3]))) == 0 || !wo.open)
			return MMSYSERR_INVALPARAM;
		t = snd_now();
		if ((long)(t - snd_taken()) > 0)
			t = snd_taken();
		t -= wo.base;
		switch (GW(p)) {
		case 1:
			PL(p + 2, (u32)(t * 1000 / (unsigned long)wo.rate));
			break;
		case 2:
			PL(p + 2, (u32)t);
			break;
		default:
			PW(p, 4);
			PL(p + 2, (u32)(t * wo.align));
		}
		return 0;
	case WODM_BREAKLOOP:
		return 0;
	}
	return MMSYSERR_NOTSUPPORTED;
}

static u32
audioproc(a)
	u32 *a;
{
	switch (a[2] & 0xffff) {
	case DRV_LOAD: case DRV_ENABLE: case DRV_DISABLE: case DRV_FREE:
	case DRV_OPEN: case DRV_CLOSE:
		return 1;
	case DRV_INSTALL: case DRV_REMOVE:
		return 1;		/* DRV_OK */
	case DRV_QUERYCONFIGURE:
		return 0;
	}
	return 0;
}

/* no recording, MIDI or auxiliary devices: their counts are 0 */
static u32 widmessage(a) u32 *a; { return (a[1] & 0xffff) == 50 ? 0 : MMSYSERR_BADDEVICEID; }
static u32 modmessage(a) u32 *a; { return (a[1] & 0xffff) == 1 ? 0 : MMSYSERR_BADDEVICEID; }
static u32 midmessage(a) u32 *a; { return (a[1] & 0xffff) == 53 ? 0 : MMSYSERR_BADDEVICEID; }
static u32 auxmessage(a) u32 *a; { return (a[1] & 0xffff) == 3 ? 0 : MMSYSERR_BADDEVICEID; }

/* ---- the work Windows does at interrupt time ---- */

void
mm_tick()
{
	u32 now;
	int i;

	if (busy)
		return;
	busy = 1;
	wavepump();
	now = w16_ticks();
	for (i = 0; i < NTEV; i++)
		if (tevs[i].used && (int)(now - tevs[i].due) >= 0) {
			u32 fn = tevs[i].fn, user = tevs[i].user;

			if (tevs[i].periodic)
				tevs[i].due = now + tevs[i].ms;
			else
				tevs[i].used = 0;
			cb_begin();
			cb_push16(i + 1);
			cb_push16(0);
			cb_push32(user);
			cb_push32((u32)0);
			cb_push32((u32)0);
			cb_call(fn, 0);
		}
	busy = 0;
}

/* milliseconds until mm_tick has something to do, -1 never */
int
mm_next()
{
	u32 now = w16_ticks();
	int i, t = -1, d;

	for (i = 0; i < NTEV; i++)
		if (tevs[i].used) {
			d = (int)(tevs[i].due - now);
			if (d < 0)
				d = 0;
			if (t < 0 || d < t)
				t = d;
		}
	if (wo.open && wo.nq && (t < 0 || t > 50))
		t = 50;
	return t;
}

/*
 * A plain beep when no sound is set for it: a short tone, unless the
 * wave device is busy.
 */
void
mm_beep()
{
	static char tone[11025 / 8];
	int i;

	if (wo.open)
		return;
	for (i = 0; i < (int)sizeof tone; i++)
		tone[i] = (i / 7) & 1 ? 0xa0 : 0x60;	/* about 790 Hz */
	snd_start(11025L, 8, 1);
	snd_put(tone, (long)sizeof tone);
}

struct impl mm_impl[] = {
	{ "TIMER", "DriverProc", timerproc },
	{ "ASHAUDIO", "DriverProc", audioproc },
	{ "ASHAUDIO", "wodMessage", wodmessage },
	{ "ASHAUDIO", "widMessage", widmessage },
	{ "ASHAUDIO", "modMessage", modmessage },
	{ "ASHAUDIO", "midMessage", midmessage },
	{ "ASHAUDIO", "auxMessage", auxmessage },
	{ 0 }
};
