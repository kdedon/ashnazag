/*
 * log.c -- the layer's log (C:\MINT\XAMINT.LOG), host errors, time.
 */
#include <stdarg.h>
#include "xamint.h"

int logfd = -1;

/* host errno, MiNT error */
static const short errtab[][2] = {
	{ 1, -38 }, { 2, -33 }, { 3, -20 }, { 4, -128 }, { 5, -90 }, { 6, -46 },
	{ 8, -66 }, { 9, -37 }, { 10, -21 }, { 11, -326 }, { 12, -39 }, { 13, -36 },
	{ 14, -40 }, { 16, -2 }, { 17, -85 }, { 18, -48 }, { 19, -15 }, { 20, -34 },
	{ 21, -24 }, { 22, -25 }, { 23, -50 }, { 24, -35 }, { 25, -87 }, { 26, -70 },
	{ 27, -71 }, { 28, -91 }, { 29, -6 }, { 30, -13 }, { 31, -82 }, { 32, -81 },
	{ 34, -88 }, { 45, -22 }, { 46, -110 }, { 78, -86 }, { 89, -32 }, { 90, -80 },
	{ 93, -83 }, { 95, -300 }, { 96, -301 }, { 97, -302 }, { 98, -303 }, { 99, -304 },
	{ 120, -305 }, { 121, -306 }, { 122, -307 }, { 123, -308 }, { 124, -309 },
	{ 125, -310 }, { 126, -311 }, { 127, -312 }, { 128, -313 }, { 129, -314 },
	{ 130, -315 }, { 131, -316 }, { 132, -327 }, { 133, -317 }, { 134, -318 },
	{ 143, -319 }, { 145, -320 }, { 146, -321 }, { 147, -322 }, { 148, -323 },
	{ 149, -324 }, { 150, -325 }, { 0, 0 }
};

/* a host call's -errno as a MiNT error; other values pass */
long
merr(long e)
{
	int i;

	if (e >= 0)
		return e;
	for (i = 0; errtab[i][0]; i++)
		if (errtab[i][0] == -e)
			return errtab[i][1];
	return EERROR;
}

void
xlog_open(void)
{
	char hp[PLEN];

	if (host_path("C:\\MINT\\XAMINT.LOG", hp, 1) == 0)
		logfd = hc(H_OPEN, 3, (long)hp, (long)(HO_WRONLY | HO_CREAT | HO_TRUNC), 0644L);
}

void
xlog(const char *fmt, ...)
{
	char buf[256];
	va_list a;
	long n;

	if (logfd < 0)
		return;
	n = ksprintf(buf, sizeof buf - 1, "%ld ", now_ms());
	va_start(a, fmt);
	n += kvsprintf(buf + n, sizeof buf - 1 - n, fmt, a);
	va_end(a);
	if (n > (long)sizeof buf - 2)
		n = sizeof buf - 2;
	buf[n++] = '\n';
	hc(H_WRITE, 3, (long)logfd, (long)buf, n);
}

/* milliseconds since boot, from TOS's 200 Hz counter */
long
now_ms(void)
{
	return *(volatile long *)0x4ba * 5;
}

/* a table slot nobody serves: 0-511 kentry, 512- GEMDOS */
long
xa_unserved(short n)
{
	static unsigned char seen[1024 / 8];

	if (!(seen[n >> 3] & (1 << (n & 7)))) {
		seen[n >> 3] |= 1 << (n & 7);
		if (n >= 512)
			xlog("unserved GEMDOS 0x%x", n - 512);
		else
			xlog("unserved kentry slot %d", n);
	}
	return ENOSYS;
}
