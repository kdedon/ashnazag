/*
 * rtctest.c -- rtc.c against a model of the Mac RTC on VIA1 port B.
 *
 * The model takes a bit on each rising clock edge while PB0 is an output,
 * drives the next read bit after each falling edge while PB0 is an input,
 * and forgets a partial command when enable goes high.  It flags protocol
 * errors and any change to port B bits 3-7 or their directions.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#define V_ORB	0x0000
#define V_DDRB	0x0400
#define DATA	0x01
#define CLK	0x02
#define ENB	0x04
#define EPOCH	2082844800UL

long sim_hrestime[2];
extern void clkset(), stime();
extern int xpram_read();
extern void xpram_write();

static int orb = 0x77, ddrb = 0x70, pin;
static int nbits, shreg, nbytes, cmd, addr, rdbyte, rdbits;
static unsigned long secs, nread;
static int wp = 1, tick_every, ignore_writes, deny;
static unsigned char xp[256];
static int bad, protowarn, notaken;
static char lastmsg[200];

static void
fail(const char *m)
{
	printf("FAIL %s\n", m);
	bad = 1;
}

static void
check(int ok, const char *m)
{
	if (ok)
		printf("OK   %s\n", m);
	else
		fail(m);
}

void
sim_printf(const char *fmt, ...)
{
	va_list ap;

	if (strstr(fmt, "did not take"))
		notaken++;
	va_start(ap, fmt);
	vsnprintf(lastmsg, sizeof lastmsg, fmt, ap);
	va_end(ap);
	printf("  | %s", lastmsg);
}

int
sim_stime_orig(long *uap, char *rvp)
{
	if (deny)
		return 1;
	sim_hrestime[0] = *uap;
	return 0;
}

int
sim_rd(int r)
{
	if (r == V_DDRB)
		return ddrb;
	if (r != V_ORB)
		fail("read of a VIA register other than ORB/DDRB");
	/* inputs: PB0 from the RTC, PB3 (ADB /INT) high */
	return (orb & ddrb) | (~ddrb & (0x08 | (pin & DATA)));
}

static void
load(int v)
{
	rdbyte = v;
	rdbits = 8;
}

static void
byte(int b)
{
	int n;

	if (++nbytes == 1) {
		cmd = b;
		if ((b & 0x78) == 0x38)
			return;			/* extended: address follows */
		if (b & 0x80) {
			if ((b & 0x73) != 0x01) {
				protowarn++;
				return;
			}
			if (tick_every && ++nread % tick_every == 0)
				secs++;
			load(secs >> 8 * (b >> 2 & 3) & 0xFF);
		}
		return;
	}
	if ((cmd & 0x78) == 0x38) {
		if (nbytes == 2) {
			if (b & 0x83)
				protowarn++;
			addr = (cmd & 7) << 5 | (b >> 2 & 0x1F);
			if (cmd & 0x80)
				load(xp[addr]);
		} else if (nbytes == 3 && !(cmd & 0x80)) {
			if (wp)
				protowarn++;
			else
				xp[addr] = b;
		} else
			protowarn++;
		return;
	}
	if (nbytes != 2 || (cmd & 0x80)) {
		protowarn++;
		return;
	}
	if (cmd == 0x35) {
		if (b != 0x55 && b != 0xD5)
			protowarn++;
		wp = b >> 7;
	} else if ((cmd & 0xF3) == 0x01) {
		n = cmd >> 2 & 3;
		if (wp)
			protowarn++;
		else if (!ignore_writes)
			secs = (secs & ~(0xFFUL << 8 * n)) | (unsigned long)b << 8 * n;
	} else
		protowarn++;
}

void
sim_wr(int r, int v)
{
	int old = orb;

	if (r == V_DDRB) {
		ddrb = v & 0xFF;
		return;
	}
	if (r != V_ORB) {
		fail("write to a VIA register other than ORB/DDRB");
		return;
	}
	orb = v & 0xFF;
	if (orb & ENB) {
		if (nbits || rdbits)
			protowarn++;
		nbits = shreg = nbytes = rdbits = 0;
		return;
	}
	if ((ddrb & DATA) && !(old & CLK) && (orb & CLK)) {
		if (rdbits)
			protowarn++;
		shreg = shreg << 1 | (orb & DATA);
		if (++nbits == 8) {
			byte(shreg & 0xFF);
			nbits = shreg = 0;
		}
	} else if (!(ddrb & DATA) && (old & CLK) && !(orb & CLK)) {
		if (!rdbits)
			protowarn++;
		else {
			pin = rdbyte >> 7 & 1;
			rdbyte <<= 1;
			rdbits--;
		}
	}
}

static void
setlong(int a, unsigned long v)
{
	int i;

	for (i = 0; i < 4; i++)
		xp[a + i] = v >> 8 * (3 - i);
}

static int
portok(void)
{
	if (protowarn || ((orb ^ 0x77) & 0x70) || (ddrb & 0xF8) != 0x70 || !(orb & ENB))
		printf("  | orb %02x ddrb %02x protocol errors %d\n", orb, ddrb, protowarn);
	return ((orb ^ 0x77) & 0x70) == 0 && (ddrb & 0xF8) == 0x70 &&
	    (orb & ENB) && protowarn == 0;
}

/* host gmtime for a unix time, in rtc.c's format */
static void
gm(long t, char *buf)
{
	time_t tt = t;
	strftime(buf, 20, "%Y-%m-%d %H:%M:%S", gmtime(&tt));
}

int
main(void)
{
	char exp[40], line[80];
	long now = 1790000000L, base = 723000000L, t;
	int i;

	/* PRAM valid, Map offset +1 h with the DST flag */
	setlong(0x0C, 0x4E754D63);
	setlong(0xEC, 0x80000E10);
	secs = now + EPOCH + 3600;
	clkset(base);
	check(sim_hrestime[0] == now, "clkset: local RTC less the Map offset (+1 h, DST flag)");
	check(portok(), "clkset: port B outputs 4-6 and all directions but 0-2 kept, protocol clean");

	setlong(0xEC, 0x00FFB9B0);		/* -18000 s */
	secs = now + EPOCH - 18000;
	clkset(base);
	check(sim_hrestime[0] == now, "clkset: negative offset sign-extended (-5 h)");

	setlong(0x0C, 0);
	secs = now + EPOCH;
	clkset(base);
	check(sim_hrestime[0] == now, "clkset: no 'NuMc', RTC taken as GMT");

	secs = 0x6A000000UL;			/* 1956: battery reset */
	clkset(base);
	check(sim_hrestime[0] == base, "clkset: RTC before the root's time, root's time used");

	tick_every = 10;
	secs = now + EPOCH;
	clkset(base);
	tick_every = 0;
	t = (long)(secs - EPOCH);
	check(sim_hrestime[0] >= now && sim_hrestime[0] <= t,
	    "clkset: a clock ticking between byte reads gives a consistent reading");
	check(portok(), "port B unchanged after the ticking reads");

	tick_every = 1;
	clkset(base);
	tick_every = 0;
	check(sim_hrestime[0] == base && strstr(lastmsg, "unstable") != 0,
	    "clkset: an RTC never reading the same twice is not used");

	setlong(0x0C, 0x4E754D63);
	setlong(0xEC, 0x00000E10);
	wp = 1;
	t = now + 12345;
	stime(&t, (char *)0);
	check(sim_hrestime[0] == t && secs == t + EPOCH + 3600,
	    "stime: kernel time and local RTC set");
	check(wp == 1, "stime: write protection back on");
	check(portok() && notaken == 0, "stime: protocol clean, no complaint");

	deny = 1;
	secs = 5;
	t = now;
	stime(&t, (char *)0);
	check(secs == 5, "stime refused: RTC untouched");
	deny = 0;

	ignore_writes = 1;
	secs = now + EPOCH;
	t = now - 86400;
	stime(&t, (char *)0);
	check(notaken == 1 && portok(), "RTC ignoring writes: one complaint, no hang");
	ignore_writes = 0;

	xpram_write(0x80, 0x5A);
	xpram_write(0xFF, 0xA5);
	check(xp[0x80] == 0x5A && xpram_read(0x80) == 0x5A && xpram_read(0xFF) == 0xA5 &&
	    xpram_read(0xEF) == 0x10 && wp == 1 && portok(), "XPRAM write and read back");

	for (i = 0; i < 5; i++) {
		static long v[] = { 0, 951782400L, 1790000000L, 2147483647L, 1234567890L };
		t = v[i];
		secs = t + EPOCH + 3600;
		setlong(0xEC, 0x00000E10);
		clkset(0L);
		gm(t, exp);
		snprintf(line, sizeof line, "rtc_fmt(%ld) = %s", t, exp);
		check(sim_hrestime[0] == t && strstr(lastmsg, exp) != 0, line);
	}
	printf("%s\n", bad ? "RTC TESTS: FAIL" : "RTC TESTS: all OK");
	return bad;
}
