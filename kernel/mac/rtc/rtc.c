/*
 * Quadra 800 real-time clock, PRAM and restart.
 *
 * The RTC hangs on VIA1 port B: PB0 data, PB1 clock, PB2 enable (low =
 * selected).  Bytes go MSB first; the RTC takes a bit on the rising clock
 * edge and drives the next one after the falling edge.
 *
 *   seconds byte n (0 = LSB)  read 0x81 | n << 2, write 0x01 | n << 2
 *   write-protect             write 0x35, then 0x55 (off) or 0xD5 (on)
 *   XPRAM byte a              0xB8 | a >> 5 (read) or 0x38 | a >> 5 (write),
 *                             then (a & 0x1F) << 2
 *
 * It counts seconds since 1904-01-01, local time.  The offset to GMT is
 * the Map setting in XPRAM 0xEC-0xEF (low 24 bits, signed, DST included),
 * trusted when XPRAM 0x0C holds 'NuMc'; otherwise the RTC is taken as GMT.
 */

#define VOL	__volatile__

#define V_ORB		0x0000
#define V_DDRB		0x0400
#define V_IER		0x1C00

#ifdef RTC_SIM
#include "rtcsim.h"
#else
#define VRD(r)		(*(VOL unsigned char *)(0x50F00000 + (r)))
#define VWR(r, v)	(VRD(r) = (v))
#define VIA2_IER	(*(VOL unsigned char *)0x50F03C00)
#define SN_CR		(*(VOL unsigned short *)0x50F0A002)
#define SN_IMR		(*(VOL unsigned short *)0x50F0A012)
#define NCR_CMD		(*(VOL unsigned char *)0x50F10030)
#endif

#define RTC_DATA	0x01
#define RTC_CLK		0x02
#define RTC_ENB		0x04

#define MAC_EPOCH	0x7C25B080	/* 1904-01-01 .. 1970-01-01, seconds */
#define XP_VALID	0x0C
#define XP_GMT		0xEC
#define NUMC		0x4E754D63	/* 'NuMc' */

extern long hrestime[];
extern int __amix_stime();
extern void dhalt(), haltsys(), mac_restart(), printf();

/* IPL 7: the ADB driver rewrites port B from its IPL 4 interrupt */
static int
rtc_splhi()
{
	int s = 0;

#ifndef RTC_SIM
	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
#endif
	return s;
}

static void
rtc_splx(s)
int s;
{
#ifndef RTC_SIM
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
#endif
}

static void
rtc_send(b)
register int b;
{
	register int i, v;

	v = VRD(V_ORB) & ~(RTC_CLK | RTC_DATA);
	for (i = 0; i < 8; i++, b <<= 1) {
		v = (b & 0x80) ? v | RTC_DATA : v & ~RTC_DATA;
		VWR(V_ORB, v);
		VWR(V_ORB, v | RTC_CLK);
	}
}

static int
rtc_recv()
{
	register int i, v, b;

	VWR(V_DDRB, VRD(V_DDRB) & ~RTC_DATA);
	v = VRD(V_ORB) & ~RTC_CLK;
	for (b = i = 0; i < 8; i++) {
		VWR(V_ORB, v);
		VWR(V_ORB, v | RTC_CLK);
		b = b << 1 | (VRD(V_ORB) & RTC_DATA);
	}
	VWR(V_DDRB, VRD(V_DDRB) | RTC_DATA);
	return b;
}

/*
 * One transaction: command c, address byte a if a >= 0, then data byte w
 * written if w >= 0, else one byte read and returned.
 */
static int
rtc_xfer(c, a, w)
int c, a, w;
{
	int s, r;

	s = rtc_splhi();
	VWR(V_ORB, VRD(V_ORB) | RTC_ENB | RTC_CLK);
	VWR(V_DDRB, VRD(V_DDRB) | RTC_ENB | RTC_CLK | RTC_DATA);
	VWR(V_ORB, VRD(V_ORB) & ~RTC_ENB);
	rtc_send(c);
	if (a >= 0)
		rtc_send(a);
	r = 0;
	if (w >= 0)
		rtc_send(w);
	else
		r = rtc_recv();
	VWR(V_ORB, VRD(V_ORB) | RTC_ENB);
	rtc_splx(s);
	return r;
}

static unsigned long
rtc_rdsec()
{
	unsigned long t;
	int n;

	for (t = 0, n = 3; n >= 0; n--)
		t = t << 8 | rtc_xfer(0x81 | n << 2, -1, -1);
	return t;
}

/* Two equal readings in a row (the bytes are read separately), else 0. */
static unsigned long
rtc_gettime()
{
	unsigned long a, b;
	int i;

	a = rtc_rdsec();
	for (i = 0; i < 4; i++, a = b)
		if ((b = rtc_rdsec()) == a)
			return a;
	return 0;
}

static void
rtc_wrsec(t)
unsigned long t;
{
	int n;

	rtc_xfer(0x35, -1, 0x55);
	for (n = 0; n < 4; n++)
		rtc_xfer(0x01 | n << 2, -1, (int)(t >> 8 * n & 0xFF));
	rtc_xfer(0x35, -1, 0xD5);
}

int
xpram_read(a)
int a;
{
	return rtc_xfer(0xB8 | (a >> 5 & 7), (a & 0x1F) << 2, -1);
}

void
xpram_write(a, v)
int a, v;
{
	rtc_xfer(0x35, -1, 0x55);
	rtc_xfer(0x38 | (a >> 5 & 7), (a & 0x1F) << 2, v & 0xFF);
	rtc_xfer(0x35, -1, 0xD5);
}

static unsigned long
xpram_long(a)
int a;
{
	unsigned long v;
	int i;

	for (v = 0, i = 0; i < 4; i++)
		v = v << 8 | xpram_read(a + i);
	return v;
}

/* Seconds east of GMT the RTC runs at */
static long
rtc_gmtdelta()
{
	long d;

	if (xpram_long(XP_VALID) != NUMC)
		return 0;
	d = xpram_long(XP_GMT) & 0xFFFFFF;
	return (d & 0x800000) ? d - 0x1000000 : d;
}

/* "YYYY-MM-DD hh:mm:ss", GMT */
static char *
rtc_fmt(t)
long t;
{
	static char buf[20];
	long z, era, doe, yoe, doy, mp, y, m, d, s;
	register char *p;
	int i;

	if (t < 0)
		return "(before 1970)";
	s = t % 86400;
	z = t / 86400 + 719468;
	era = z / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	y = yoe + era * 400 + (m <= 2);
	p = buf;
	for (i = 1000; i; i /= 10)
		*p++ = '0' + y / i % 10;
	*p++ = '-'; *p++ = '0' + m / 10; *p++ = '0' + m % 10;
	*p++ = '-'; *p++ = '0' + d / 10; *p++ = '0' + d % 10;
	*p++ = ' '; *p++ = '0' + s / 36000; *p++ = '0' + s / 3600 % 10;
	*p++ = ':'; *p++ = '0' + s / 600 % 6; *p++ = '0' + s / 60 % 10;
	*p++ = ':'; *p++ = '0' + s % 60 / 10; *p++ = '0' + s % 10;
	*p = 0;
	return buf;
}

/*
 * The root mount's time-of-day hook, given the root's last write time.
 * The RTC wins unless it reads unstable or earlier than that (a dead
 * battery reads 1904 or 1956).
 */
void
clkset(base)
long base;
{
	unsigned long m;
	long d, t;

	m = rtc_gettime();
	d = rtc_gmtdelta();
	t = m - MAC_EPOCH - d;
	if (m == 0) {
		printf("mac: RTC unstable, not used\n");
		t = base;
	} else if (t < base) {
		printf("mac: RTC %s GMT is before the root's last write, not used\n",
		    rtc_fmt(t));
		t = base;
	} else
		printf("mac: time %s GMT from the RTC (%d s from GMT)\n",
		    rtc_fmt(t), (int)d);
	hrestime[0] = t;
}

/* The RTC to Unix time t, in local time; read back, retried once. */
static void
rtc_settime(t)
long t;
{
	unsigned long m, r;
	int i;

	m = t + MAC_EPOCH + rtc_gmtdelta();
	for (i = 0; i < 2; i++) {
		rtc_wrsec(m);
		r = rtc_gettime();
		if (r - m <= 2)
			return;
	}
	printf("mac: the RTC did not take the new time\n");
}

/* stime(2), also setting the RTC */
int
stime(uap, rvp)
char *uap, *rvp;
{
	int e;

	if ((e = __amix_stime(uap, rvp)) == 0)
		rtc_settime(hrestime[0]);
	return e;
}

#ifndef RTC_SIM
/*
 * Interrupts and SONIC DMA off, SCSI chip and bus reset (a disconnected
 * target would otherwise reselect into the ROM), then the ROM's restart
 * entry.
 */
void
mac_reboot()
{
	register int i;

	printf("\nRestarting the system.\n");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (0x2700) : "memory");
	VWR(V_IER, 0x7F);
	VIA2_IER = 0x7F;
	SN_IMR = 0;
	/* reset, then leave reset: the ROM's Ethernet start-up stalls with RST set */
	SN_CR = 0x80;
	for (i = 0; i < 1000; i++)
		(void)SN_CR;
	SN_CR = 0;
	NCR_CMD = 0x02;
	NCR_CMD = 0x00;
	NCR_CMD = 0x03;
	NCR_CMD = 0x00;
	mac_restart();
}

/* uadmin(A_SHUTDOWN or A_REBOOT, fcn): AD_HALT halts, the rest restart. */
void
mdboot(fcn, mdep)
int fcn, mdep;
{
	dhalt();
	if (fcn == 0)
		haltsys(0);
	mac_reboot();
}
#endif
