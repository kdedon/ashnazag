/*
 * atartc.c -- Falcon real-time clock (MC146818 compatible).
 *
 * Index register at 0xFFFF8961, data at 0xFFFF8963.  Year 0 is 1968, as
 * TOS keeps it.  Like TOS, the clock keeps local time; setclk converts
 * through /dev/clock, which reads and writes its wall time as seconds
 * from 1970.
 */

#include "sys/types.h"
#include "sys/errno.h"
#include "sys/uio.h"

#define RTC_IDX		(*(volatile unsigned char *)0xFFFF8961)
#define RTC_DATA	(*(volatile unsigned char *)0xFFFF8963)

#define R_SEC	0
#define R_MIN	2
#define R_HOUR	4
#define R_WDAY	6
#define R_MDAY	7
#define R_MON	8
#define R_YEAR	9
#define R_A	10
#define R_B	11
#define A_UIP	0x80
#define B_SET	0x80
#define B_DM	0x04		/* binary, not BCD */
#define B_24H	0x02
#define YEAR0	1968

extern long hrestime[];
extern int printf(), uiomove();

static int
rtc_rd(r)
int r;
{
	RTC_IDX = r;
	return RTC_DATA;
}

static void
rtc_wr(r, v)
int r, v;
{
	RTC_IDX = r;
	RTC_DATA = v;
}

/* days from 1970-01-01 to y-m-d */
static long
rtc_days(y, m, d)
long y, m, d;
{
	long era, yoe, doy;

	y -= m <= 2;
	era = y / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

/* one consistent reading as Unix time, or -1 */
static long
rtc_gettime()
{
	int r[10], b, i, n, pm;

	for (n = 0; n < 2; n++) {
		for (i = 100000; i && (rtc_rd(R_A) & A_UIP); i--)
			;
		for (i = 0; i < 10; i++)
			r[i] = rtc_rd(i);
		if (r[R_SEC] == rtc_rd(R_SEC))
			break;
	}
	b = rtc_rd(R_B);
	pm = !(b & B_24H) && (r[R_HOUR] & 0x80);
	r[R_HOUR] &= 0x7F;
	if (!(b & B_DM))
		for (i = 0; i < 10; i++)
			r[i] = (r[i] >> 4) * 10 + (r[i] & 15);
	if (!(b & B_24H))
		r[R_HOUR] = r[R_HOUR] % 12 + (pm ? 12 : 0);
	if (r[R_SEC] > 59 || r[R_MIN] > 59 || r[R_HOUR] > 23 || r[R_YEAR] > 99 ||
	    r[R_MDAY] < 1 || r[R_MDAY] > 31 || r[R_MON] < 1 || r[R_MON] > 12)
		return -1;
	return ((rtc_days((long)r[R_YEAR] + YEAR0, (long)r[R_MON],
	    (long)r[R_MDAY]) * 24 + r[R_HOUR]) * 60 + r[R_MIN]) * 60 + r[R_SEC];
}

static int
rtc_enc(v, b)
int v, b;
{
	return (b & B_DM) ? v : v / 10 << 4 | v % 10;
}

static int
rtc_settime(t)
long t;
{
	long z, era, doe, yoe, doy, mp, y, m, d, s;
	int b, h;

	s = t % 86400;
	z = t / 86400 + 719468;
	era = z / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	y = yoe + era * 400 + (m <= 2) - YEAR0;
	if (t < 0 || y > 99)
		return -1;
	b = rtc_rd(R_B);
	rtc_wr(R_B, b | B_SET);
	h = s / 3600;
	if (b & B_24H)
		h = rtc_enc(h, b);
	else
		h = rtc_enc(h % 12 ? h % 12 : 12, b) | (h >= 12 ? 0x80 : 0);
	rtc_wr(R_SEC, rtc_enc((int)(s % 60), b));
	rtc_wr(R_MIN, rtc_enc((int)(s / 60 % 60), b));
	rtc_wr(R_HOUR, h);
	rtc_wr(R_WDAY, rtc_enc((int)((t / 86400 + 4) % 7 + 1), b));
	rtc_wr(R_MDAY, rtc_enc((int)d, b));
	rtc_wr(R_MON, rtc_enc((int)m, b));
	rtc_wr(R_YEAR, rtc_enc((int)y, b));
	rtc_wr(R_B, b & ~B_SET);
	return 0;
}

/*
 * The root mount's time-of-day hook, given the root's last write time.
 * The RTC wins unless it reads invalid or more than a day earlier: it
 * is off by the zone offset until setclk runs.
 */
void
clkset(base)
long base;
{
	long t;

	t = rtc_gettime();
	if (t < 0 || t < base - 86400) {
		printf("atari: RTC %s, using the root's time\n",
		    t < 0 ? "invalid" : "before the root's last write");
		t = base;
	}
	hrestime[0] = t;
}

/* /dev/clock: one four-byte reading per open */
/* ARGSUSED */
int
rtcread(dev, uiop, cr)
dev_t dev;
struct uio *uiop;
char *cr;
{
	long t;

	if (uiop->uio_offset != 0)
		return 0;
	if (uiop->uio_resid < sizeof t)
		return EINVAL;
	if ((t = rtc_gettime()) < 0)
		return EIO;
	return uiomove((caddr_t)&t, sizeof t, UIO_READ, uiop);
}

/* ARGSUSED */
int
rtcwrite(dev, uiop, cr)
dev_t dev;
struct uio *uiop;
char *cr;
{
	long t;
	int e;

	if (uiop->uio_resid != sizeof t)
		return EINVAL;
	if ((e = uiomove((caddr_t)&t, sizeof t, UIO_WRITE, uiop)) != 0)
		return e;
	return rtc_settime(t) ? EINVAL : 0;
}

/* uadmin(A_SHUTDOWN or A_REBOOT, fcn): AD_HALT halts, the rest restart
 * through the ROM's reset entry, which boots the disk again. */
extern void dhalt(), haltsys(), ata_restart();

void
mdboot(fcn, mdep)
int fcn, mdep;
{
	dhalt();
	if (fcn == 0)
		haltsys(0);
	ata_restart();
}
