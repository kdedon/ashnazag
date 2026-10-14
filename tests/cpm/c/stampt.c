/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * stampt.c - Exercise the label mode, the clock and update stamps
 * (functions 101, 105, 102).  The clock is the host's.
 */

#include "cpm.h"

static struct fcb	f;
static char		buf[SECLEN];
static char		tod[4];
static int		bad;

static VOID	putbcd();
static VOID	puthex();

int main(argc, argv)
int argc;
char *argv[];
{
	register int	r, day;
	unsigned char	*u;

	r = __bdos(BDOS_GETLABEL, 0L) & 0xff;
	cputs("STAMPT: fn 101 label mode ");
	puthex(r);
	if (!(r & DL_EXISTS) || !(r & DL_UPDATE)) {
		cputs("  BAD -- no update stamps");
		bad++;
	}
	cputs("\r\n");

	r = __bdos(BDOS_GETTIME, (long) tod) & 0xff;
	day = (tod[0] & 0xff) | (tod[1] & 0xff) << 8;
	cputs("STAMPT: fn 105 day ");
	putdec((unsigned) day);
	cputs(" ");
	putbcd(tod[2] & 0xff);
	cputs(":");
	putbcd(tod[3] & 0xff);
	cputs(" sec ");
	putbcd(r);
	if (day < 2922 || (tod[2] & 0xff) > 0x23 || r > 0x59) {	/* 1986 on */
		cputs("  BAD");
		bad++;
	}
	cputs("\r\n");

	mkfcb("STAMPT.TXT", &f);
	__bdos(BDOS_DELETE, (long) &f);
	mkfcb("STAMPT.TXT", &f);
	setdma(buf);
	if ((__bdos(BDOS_MAKE, (long) &f) & 0xff) == 0xff) {
		cputs("STAMPT: BAD -- cannot create STAMPT.TXT\r\n");
		return (1);
	}
	buf[0] = 'a';
	__bdos(BDOS_WRITESEQ, (long) &f);
	__bdos(BDOS_CLOSE, (long) &f);

	/* function 102: the update stamp in FCB bytes 28-31 */
	mkfcb("STAMPT.TXT", &f);
	r = __bdos(BDOS_RDSTAMPS, (long) &f) & 0xff;
	u = (unsigned char *) &f + 28;
	cputs("STAMPT: update day ");
	putdec((unsigned) (u[0] | u[1] << 8));
	if (r != 0 || (u[0] | u[1] << 8) < day || (u[0] | u[1] << 8) > day + 1) {
		cputs("  BAD");
		bad++;
	}
	cputs("\r\n");

	/* a wildcard is error 9 */
	__bdos(BDOS_ERRMODE, (long) ERRMODE_RETURN);
	mkfcb("STAMP?.TXT", &f);
	r = __bdos(BDOS_RDSTAMPS, (long) &f);
	__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);
	cputs("STAMPT: fn 102 on a wildcard -> ");
	putdec((unsigned) ((r >> 8) & 0xff));
	cputs("/");
	putdec((unsigned) (r & 0xff));
	if (((r >> 8) & 0xff) != 9 || (r & 0xff) != 0xff) {
		cputs("  BAD -- want error 9, return 255");
		bad++;
	}
	cputs("\r\n");
	mkfcb("STAMPT.TXT", &f);
	__bdos(BDOS_DELETE, (long) &f);
	cputs(bad ? "STAMPT: FAIL\r\n" : "STAMPT: PASS\r\n");
	return (bad != 0);
}

static VOID putbcd(v)
register int v;
{
	conout('0' + ((v >> 4) & 0x0f));
	conout('0' + (v & 0x0f));
}

static VOID puthex(v)
register int v;
{
	static char	hex[] = "0123456789abcdef";

	conout(hex[(v >> 4) & 0x0f]);
	conout(hex[v & 0x0f]);
}
