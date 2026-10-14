/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * date.c - DATE, the CP/M 3 date/time utility, for the C900.
 *
 *	DATE			show the date and time once
 *	DATE C			show it continuously until a key is hit
 *	DATE SET		prompt for the date and the time
 *	DATE MM/DD/YY HH:MM:SS	set it from the command line
 *
 * Behaviour follows DRI's date.plm; the display is v3's
 * "Www MM/DD/YY HH:MM:SS".
 *
 * The clock is the host's, read with BDOS function 105; it cannot be set
 * from here.
 *
 * The TOD block is {date-word, hour, minute, second}: days since
 * 1977-12-31, high byte first, then BCD.
 */

#include "cpm.h"

#define	TOD_DATEHI	0
#define	TOD_DATELO	1
#define	TOD_HOUR	2
#define	TOD_MIN		3
#define	TOD_SEC		4
#define	TODLEN		5

#define	RTC_NONE	0xff		/* fn 23 status: no clock	*/

#define	BASEYEAR	1978		/* date word 1 = 1978-01-01	*/
#define	LASTYEAR	2077		/* the 16-bit word runs out here */

static char	tod[TODLEN];
static char	line[130];
static char	dsave[16];		/* line[] is reused by the 2nd prompt */

static char	dpm[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
static char	*wdname[] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/* set == 0 reads the clock into tod[]; the host's clock is never set */
static int biostime(set)
int set;
{
	char	dat[4];

	if (set)
		return (RTC_NONE);
	tod[TOD_SEC] = __bdos(BDOS_GETTIME, (long) dat);
	tod[TOD_DATELO] = dat[0];	/* the date word is stored low byte first */
	tod[TOD_DATEHI] = dat[1];
	tod[TOD_HOUR] = dat[2];
	tod[TOD_MIN] = dat[3];
	return (0);
}

static int isleap(y)
int y;
{
	return ((y & 3) == 0);		/* exact over 1978..2077	*/
}

static int mlen(m, y)
int m, y;
{
	if (m == 2 && isleap(y))
		return (29);
	return (dpm[m - 1]);
}

/* calendar date -> date word; 0 if out of range */
static unsigned ymd2day(y, m, d)
int y, m, d;
{
	register long	n;
	register int	i;

	if (y < BASEYEAR || y > LASTYEAR || m < 1 || m > 12)
		return (0);
	if (d < 1 || d > mlen(m, y))
		return (0);
	n = 0L;
	for (i = BASEYEAR; i < y; i++)
		n += isleap(i) ? 366L : 365L;
	for (i = 1; i < m; i++)
		n += (long) mlen(i, y);
	return ((unsigned) (n + (long) d));
}

/* date word -> calendar date; 0 if the word is 0 or out of range */
static int day2ymd(day, yp, mp, dp)
unsigned day;
int *yp, *mp, *dp;
{
	register long	n;
	register int	y, m;

	if (day == 0)
		return (0);
	n = (long) day;
	y = BASEYEAR;
	while (n > (isleap(y) ? 366L : 365L)) {
		n -= isleap(y) ? 366L : 365L;
		if (++y > LASTYEAR)
			return (0);
	}
	m = 1;
	while (n > (long) mlen(m, y)) {
		n -= (long) mlen(m, y);
		m++;
	}
	*yp = y;
	*mp = m;
	*dp = (int) n;
	return (1);
}

static VOID put2(n)
int n;
{
	conout('0' + (n / 10) % 10);
	conout('0' + n % 10);
}

static VOID putbcd(b)
int b;
{
	conout('0' + ((b >> 4) & 0x0f));
	conout('0' + (b & 0x0f));
}

/*
 * "Www MM/DD/YY HH:MM:SS" from the TOD block.  A zero date word means
 * the clock has never been set.
 */
static VOID show()
{
	unsigned	day;
	int		y, m, d;

	day = ((unsigned) (tod[TOD_DATEHI] & 0xff) << 8)
	    | (unsigned) (tod[TOD_DATELO] & 0xff);
	if (!day2ymd(day, &y, &m, &d)) {
		cputs("date not set");
		return;
	}
	cputs(wdname[(day - 1) % 7]);
	conout(' ');
	put2(m);
	conout('/');
	put2(d);
	conout('/');
	put2(y % 100);
	conout(' ');
	putbcd(tod[TOD_HOUR] & 0xff);
	conout(':');
	putbcd(tod[TOD_MIN] & 0xff);
	conout(':');
	putbcd(tod[TOD_SEC] & 0xff);
}

/* two digits at *pp, then one separator character; -1 on any mismatch */
static int field(pp, sep)
char **pp;
int sep;
{
	register char	*p;
	register int	v;

	p = *pp;
	if (*p < '0' || *p > '9' || p[1] < '0' || p[1] > '9')
		return (-1);
	v = (*p - '0') * 10 + (p[1] - '0');
	p += 2;
	if (sep != 0) {
		if (*p != sep)
			return (-1);
		p++;
	}
	*pp = p;
	return (v);
}

/*
 * Parse "MM/DD/YY" and "HH:MM:SS" into the TOD block.  The two-digit year
 * windows the same way the driver does: 78..99 are 19yy, 00..77 are 20yy.
 * Returns 0 on a syntax or range error.
 */
static int parse(ds, ts)
char *ds, *ts;
{
	int		m, d, y, hh, mm, ss;
	unsigned	day;

	if ((m = field(&ds, '/')) < 0)
		return (0);
	if ((d = field(&ds, '/')) < 0)
		return (0);
	if ((y = field(&ds, 0)) < 0 || *ds != 0)
		return (0);
	y += (y >= 78) ? 1900 : 2000;
	if ((day = ymd2day(y, m, d)) == 0)
		return (0);

	if ((hh = field(&ts, ':')) < 0)
		return (0);
	if ((mm = field(&ts, ':')) < 0)
		return (0);
	if ((ss = field(&ts, 0)) < 0 || *ts != 0)
		return (0);
	if (hh > 23 || mm > 59 || ss > 59)
		return (0);

	tod[TOD_DATEHI] = (char) (day >> 8);
	tod[TOD_DATELO] = (char) (day & 0xff);
	tod[TOD_HOUR] = (char) (((hh / 10) << 4) | (hh % 10));
	tod[TOD_MIN] = (char) (((mm / 10) << 4) | (mm % 10));
	tod[TOD_SEC] = (char) (((ss / 10) << 4) | (ss % 10));
	return (1);
}

/* one line from the console through BDOS function 10 */
static char *getline()
{
	register int	n;

	line[0] = 120;
	line[1] = 0;
	__bdos(BDOS_RDCONBUF, (long) line);
	n = line[1] & 0x7f;
	line[n + 2] = 0;
	cputs("\r\n");
	return (&line[2]);
}

static VOID usage()
{
	cputs("Usage: DATE [C | SET | MM/DD/YY HH:MM:SS]\r\n");
}

int main(argc, argv)
int argc;
char *argv[];
{
	register int	rc;
	register int	i;
	char		*ds;

	if (argc > 1 && argv[1][0] == 'S') {		/* DATE SET */
		cputs("Enter today's date (MM/DD/YY): ");
		ds = getline();
		for (i = 0; i < 15 && ds[i] != 0; i++)
			dsave[i] = ds[i];
		dsave[i] = 0;
		cputs("Enter the time (HH:MM:SS): ");
		if (!parse(dsave, getline())) {
			cputs("Bad date or time\r\n");
			return (1);
		}
		rc = biostime(1);
		if (rc == RTC_NONE) {
			cputs("The host's clock is not set from here\r\n");
			return (1);
		}
	} else if (argc > 2) {				/* DATE mm/dd/yy hh:mm:ss */
		if (!parse(argv[1], argv[2])) {
			usage();
			return (1);
		}
		rc = biostime(1);
		if (rc == RTC_NONE) {
			cputs("The host's clock is not set from here\r\n");
			return (1);
		}
	} else if (argc > 1 && argv[1][0] != 'C') {
		usage();
		return (1);
	}

	/* DATE C loops until a key is pressed; every other form prints once */
	for (;;) {
		rc = biostime(0);
		if (rc == RTC_NONE) {
			cputs("No clock in this machine\r\n");
			return (1);
		}
		show();
		cputs("\r\n");
		if (!(argc > 1 && argv[1][0] == 'C'))
			break;
		if (__bdos(BDOS_CONST, 0L) != 0) {
			__bdos(BDOS_CONIN, 0L);
			break;
		}
	}
	return (0);
}
