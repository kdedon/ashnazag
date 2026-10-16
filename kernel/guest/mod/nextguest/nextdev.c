/*
 * nextdev.c -- the board's registers: system control, interrupt status
 * and mask, timers, the clock chip and NVRAM.  Registers of absent
 * devices read as zero and ignore writes.
 *
 * Licence: GPL-2.0-or-later.  Derived in part from the Previous
 * emulator's register models; see THIRD_PARTY.md.
 *
 * K&R C.
 */

#include "next.h"

extern timestruc_t hrestime;

#define	ZEROBITS	0xc22e7600UL
#define	SCR1		0xf0004000UL
#define	TMC_VROW	0x118UL		/* the turbo's video registers: row bytes (4 pixels each) */
#define	TMC_VHIGH	0x340UL		/* and rows of the screen */
#define	TMC_SCR1	0xffff4fcfUL	/* the id as the turbo's memory controller reports it */

struct nx nx;

unsigned long
nx_us()
{
	timestruc_t t = hrestime;

	return (unsigned long)t.tv_sec * 1000000UL + (unsigned long)t.tv_nsec / 1000;
}

/* the highest level asked for, 0 none */
int
nx_level()
{
	unsigned long i = nx.stat & nx.mask;

	if (!i)
		return 0;
	if (i & 0xc0000000UL || (i & INT_TIMER && nx.scr2[2] & 0x80))
		return 7;
	if (i & 0x3ffc0000UL)
		return 6;
	if (i & 0x00038000UL)
		return 5;
	if (i & 0x00004000UL)
		return 4;
	if (i & 0x00003ffcUL)
		return 3;
	if (i & 2)
		return 2;
	return 1;
}

void
nx_setint(bits)
	unsigned long bits;
{
	nx.stat |= bits;
}

void
nx_clrint(bits)
	unsigned long bits;
{
	nx.stat &= ~bits;
}

static void
census(a, sz, wr, v)
	unsigned long a, v;
	int sz, wr;
{
	struct nx_census *c;
	int i;

	nx.st.lasta = a;
	nx.st.lastpc = GR_PC(u.u_ar0);
	if (a < NX_IO || a >= 0x03000000UL)
		a &= ~0xffffUL;
	for (i = 0; i < nx.st.ncensus; i++)
		if (nx.st.census[i].addr == a && nx.st.census[i].write == wr)
			return;
	if (nx.st.ncensus >= NX_NCENSUS)
		return;
	c = &nx.st.census[nx.st.ncensus++];
	c->addr = a;
	c->pc = GR_PC(u.u_ar0);
	c->value = v;
	c->size = sz;
	c->write = wr;
}

/* ---- clock chip, one bit at a time ---- */

static unsigned char *
rtcram(a)
	int a;
{
	return &nx.st.nvram[(a & 0x40 ? 32 : 0) + (a & 0x1f)];
}

static unsigned long
rtcstep(bit)
	int bit;
{
	nx.rtc_phase++;
	if (nx.rtc_phase <= 8)
		nx.rtc_addr = nx.rtc_addr << 1 | (bit ? 1 : 0);
	else {
		if (nx.rtc_phase == 9 && !(nx.rtc_addr & 0x80)) {
			if (nx.rtc_addr & 0x20) {
				switch (nx.rtc_addr & 0x7f) {
				case 0x20: nx.rtc_val = nx.rtc_time >> 24; break;
				case 0x21: nx.rtc_val = nx.rtc_time >> 16; break;
				case 0x22: nx.rtc_val = nx.rtc_time >> 8; break;
				case 0x23: nx.rtc_val = nx.rtc_time; break;
				case 0x24: nx.rtc_val = nx.rtc_alarm >> 24; break;
				case 0x25: nx.rtc_val = nx.rtc_alarm >> 16; break;
				case 0x26: nx.rtc_val = nx.rtc_alarm >> 8; break;
				case 0x27: nx.rtc_val = nx.rtc_alarm; break;
				case 0x30: nx.rtc_val = (nx.rtc_stat & 0x3f) | 0x80; break;
				case 0x31: nx.rtc_val = nx.rtc_ctl & ~0x4d; break;
				default: nx.rtc_val = 0;
				}
			} else
				nx.rtc_val = *rtcram(nx.rtc_addr);
		}
		if (nx.rtc_addr & 0x80)
			nx.rtc_val = nx.rtc_val << 1 | (bit ? 1 : 0);
		else
			bit = nx.rtc_val & (1 << (16 - nx.rtc_phase)) ? 1 : 0;
		if (nx.rtc_phase == 16) {
			if (nx.rtc_addr & 0x80) {
				if (!(nx.rtc_addr & 0x20)) {
					if (*rtcram(nx.rtc_addr) != nx.rtc_val) {
						*rtcram(nx.rtc_addr) = nx.rtc_val;
						nx.st.nvseq++;
					}
				} else
					switch (nx.rtc_addr & 0x7f) {
					case 0x20: nx.rtc_time = (nx.rtc_time & 0x00ffffffUL) | ((unsigned long)nx.rtc_val << 24); break;
					case 0x21: nx.rtc_time = (nx.rtc_time & 0xff00ffffUL) | ((unsigned long)nx.rtc_val << 16); break;
					case 0x22: nx.rtc_time = (nx.rtc_time & 0xffff00ffUL) | ((unsigned long)nx.rtc_val << 8); break;
					case 0x23: nx.rtc_time = (nx.rtc_time & 0xffffff00UL) | nx.rtc_val; break;
					case 0x31:
						nx.rtc_ctl = nx.rtc_val;
						if (nx.rtc_val & 4)
							nx.rtc_stat &= ~0x10;
						if (nx.rtc_val & 8)
							nx.rtc_stat &= ~2;
						if (nx.rtc_val & 1)
							nx.rtc_stat &= ~1;
						break;
					}
			}
			nx.rtc_addr = nx.rtc_addr == 0x7f ? 0 : nx.rtc_addr == 0xff ? 0x80 : nx.rtc_addr + 1;
			nx.rtc_phase -= 8;
		}
	}
	return bit;
}

static void
rtcreset()
{
	nx.rtc_phase = 0;
	nx.rtc_addr = nx.rtc_val = 0;
	nx.rtc_data = 0;
}

/* ---- byte registers ---- */

static int
rdb(o)
	unsigned long o;
{
	unsigned long d;
	int v;

	switch (o >> 8) {
	case 0xd0:
		if ((o & 3) == 2)
			return (nx.scr2[2] & ~4) | (nx.rtc_data ? 4 : 0);
		return nx.scr2[o & 3];
	case 0x160:
		switch (o & 15) {
		case 0: case 1:
			d = nx.hc_latch;
			if (nx.hc_csr & 0x80 && nx.hc_latch) {
				d = nx_us() - nx.hc_due;
				d = (long)d >= 0 ? 0 : -d;
				if (d > nx.hc_latch)
					d = nx.hc_latch;
			}
			return o & 1 ? d & 0xff : d >> 8 & 0xff;
		case 4:
			v = nx.hc_csr;
			nx_clrint(INT_TIMER);
			return v;
		}
		return 0;
	}
	return 0;
}

static void
wrb(o, v)
	unsigned long o;
	int v;
{
	int ch;

	v &= 0xff;
	switch (o >> 8) {
	case 0xd0:
		ch = nx.scr2[o & 3] ^ v;
		nx.scr2[o & 3] = v;
		switch (o & 3) {
		case 0:
			if (ch & 1) {
				if (v & 1) nx_setint(INT_SOFT1); else nx_clrint(INT_SOFT1);
			}
			if (ch & 2) {
				if (v & 2) nx_setint(INT_SOFT2); else nx_clrint(INT_SOFT2);
			}
			break;
		case 2:
			if (v & 1) {
				if (ch & 1) {
					nx.rtc_time = nx.rtc_ctl & 0x80 ? nx_us() / 1000000UL : nx.rtc_time;
					nx.rtc_phase = 0;
				}
				if (ch & 2 && !(v & 2))
					nx.rtc_data = rtcstep(v & 4);
			} else if (ch & 1)
				rtcreset();
			break;
		}
		return;
	case 0x160:
		switch (o & 15) {
		case 0: nx.hc_latch = (nx.hc_latch & 0xff) | (v << 8); break;
		case 1: nx.hc_latch = (nx.hc_latch & 0xff00) | v; break;
		case 4:
			ch = nx.hc_csr ^ v;
			nx.hc_csr = v & ~0x40;
			if (v & 0xc0 && (v & 0x40 || ch & 0x80))
				nx.hc_due = nx_us() + nx.hc_latch;
			nx_clrint(INT_TIMER);
			break;
		}
		return;
	}
}

static int
islong(w)
	unsigned long w;
{
	return w == 0x7000 || w == 0x7800 || w == 0xc000 || w == 0xc800 || w == 0x1a000 || w == 0x1f000 || w == 0x1f004;
}

static unsigned long
rdw(w)
	unsigned long w;
{
	unsigned long v;

	switch (w) {
	case 0x7000: return nx.stat;
	case 0x7800: return nx.mask & ~ZEROBITS;
	case 0xc000: case 0xc800: return SCR1;
	case 0x1a000:
		v = (nx_us() - nx.evoff) & 0xfffff;
		if ((long)(v - nx.evlast) <= 0)
			v = nx.evlast + 1;
		nx.evlast = v;
		return v & 0xfffff;
	}
	return 0;
}

static void
wrl(w, v)
	unsigned long w, v;
{
	switch (w) {
	case 0x7800:
		nx.mask = v | ZEROBITS;
		return;
	case 0x1a000:
		nx.evoff = nx_us();
		nx.evlast = 0;
		return;
	case 0x1f000:
		nx.st.monexit = v | 0x80000000UL;
		psignal(nx.proc, SIGUSR1);
		return;
	case 0x1f004:
		psignal(nx.proc, SIGPWR);
		return;
	}
}

/* the register at a, sz bytes; 0 done */
int
nx_read(a, sz, vp)
	unsigned long a, *vp;
	int sz;
{
	unsigned long v = 0, w, o = a & 0x1ffff;
	int i, s = nx_splhi();

	nx.st.io++;
	if ((a & 0xfffe0000UL) == NX_IO || (a & 0xfffe0000UL) == 0x02100000UL) {
		w = o & ~3UL;
		if (islong(w)) {
			v = rdw(w);
			if (sz == 2)
				v = v >> (16 - 8 * (o & 2)) & 0xffff;
			else if (sz == 1)
				v = v >> (24 - 8 * (o & 3)) & 0xff;
		} else
			for (i = 0; i < sz; i++)
				v = v << 8 | rdb(o + i);
	} else if ((a & 0xfffffffcUL) == 0x02200000UL && (a & 3) + sz <= 4) {
		/* the turbo's mirror of the machine id */
		v = TMC_SCR1 >> (32 - 8 * ((a & 3) + sz));
		if (sz < 4)
			v &= (1UL << 8 * sz) - 1;
	} else if ((a & 0xfffffff8UL) == 0x02200088UL && (a & 3) + sz <= 4) {
		/* the screen's size, which the kernel's console reads */
		v = (a & 4 ? TMC_VHIGH : TMC_VROW) >> (32 - 8 * ((a & 3) + sz)) & 0xffffffffUL;
		if (sz < 4)
			v &= (1UL << 8 * sz) - 1;
	}
	if (nx.flags & NXF_CENSUS)
		census(a, sz, 0, v);
	nx_splx(s);
	*vp = v;
	return 0;
}

int
nx_write(a, sz, v)
	unsigned long a, v;
	int sz;
{
	unsigned long o = a & 0x1ffff;
	int i, s = nx_splhi();

	nx.st.io++;
	if ((a & 0xfffe0000UL) == NX_IO || (a & 0xfffe0000UL) == 0x02100000UL) {
		if (sz == 4 && islong(o & ~3UL) && !(o & 3))
			wrl(o, v);
		else
			for (i = 0; i < sz; i++)
				wrb(o + i, (int)(v >> 8 * (sz - 1 - i)) & 0xff);
	}
	if (nx.flags & NXF_CENSUS)
		census(a, sz, 1, v);
	nx_kick();
	nx_splx(s);
	return 0;
}

/* power-on state; NVRAM and the census stay */
void
nx_reset()
{
	nx.stat = 0;
	nx.mask = ZEROBITS;
	bzero((caddr_t)nx.scr2, sizeof nx.scr2);
	nx.scr2[2] = 0x10;
	nx.scr2[3] = 0x80;
	nx.hc_csr = 0;
	nx.hc_latch = 0;
	nx.evoff = nx_us();
	nx.evlast = 0;
	rtcreset();
	nx.rtc_ctl = 0x80;
	nx.rtc_stat = 0;
	nx.rtc_time = nx_us() / 1000000UL;
	nx.rtc_alarm = 0;
}
