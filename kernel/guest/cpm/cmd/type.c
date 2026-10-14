/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * type.c - TYPE, the CP/M 3 text file lister, for the C900.
 *
 *	TYPE			prompt for a name
 *	TYPE d:name.typ[;pw]	list the file, or each match under its name
 *	TYPE name [PAGE]	pause every screen (the default)
 *	TYPE name [NO PAGE]	do not pause; NOPAGE also works
 *
 * Without an option the console page mode decides.  At the pause ^C
 * stops and ^O skips the rest of the file.
 */

#include "cpm.h"

#define	BDOS_PARSE	152

#define	MAXNAMES	1024

static char	fcb[36];
static char	cur[36];
static char	dirbuf[SECLEN];
static char	passwd[8];
static char	names[MAXNAMES][11];
static char	tb[SECLEN + 2];
static int	scbpb[2];
static int	cnt, tcnt, eod;

static VOID quit()
{
	__bdos(BDOS_WBOOT, 0L);
}

static VOID crlf()
{
	cputs("\r\n");
}

static int scbgetb(off)
int off;
{
	char	*p;

	p = (char *) scbpb;
	p[0] = (char) off;
	p[1] = 0;
	p[2] = 0;
	p[3] = 0;
	return (__bdos(49, (long) scbpb) & 0xff);
}

/*
 * Function 152 on s; returns the delimiter's offset in s, 0 at the end
 * of the line, or -1 for an invalid name.
 */
static int parse(s, f)
char *s;
char *f;
{
	long		pb[2];
	register int	i;
	register unsigned r;

	for (i = 0; i < 36; i++)
		f[i] = 0;
	pb[0] = (long) s;
	pb[1] = (long) f;
	r = __bdos(BDOS_PARSE, (long) pb);
	if (r == 0)
		return (0);
	if (r == 0xffff)
		return (-1);
	return (int) (r - ((unsigned) (long) s & 0xffff));
}

static int ucase(c)
register int c;
{
	return (c >= 'a' && c <= 'z') ? c - 0x20 : c;
}

static int rawin()
{
	register int	c;

	while ((c = __bdos(6, 0xffL) & 0xff) == 0)
		;
	return (c);
}

static VOID error(code)
int code;
{
	if (code == 0) {
		cputs("No File");
		quit();
	}
	if (code == 1) {
		cputs("\r\nBDOS Bad Sector");
		quit();
	}
	if (code == 4) {
		cputs("\r\nInvalid Drive");
		quit();
	}
	if (code == 5)
		cputs("Currently Opened");
	if (code == 7)
		cputs("Password Error");
}

static VOID erropt(code)
int code;
{
	cputs("ERROR: ");
	if (code == 0)
		cputs("Invalid or missing delimiter(s) ");
	else
		cputs("Try 'PAGE' or 'NO PAGE' ");
	quit();
}

static VOID pfile(f)
register char *f;
{
	register int	k, c;

	if (f[0] != 0) {
		conout(f[0] + '@');
		conout(':');
	}
	for (k = 1; k <= 11; k++) {
		if (k == 9)
			conout('.');
		if ((c = f[k] & 0x7f) != ' ')
			conout(c);
	}
}

static VOID getpasswd()
{
	register int	i, c;

	crlf();
	crlf();
	cputs("Password: ");
retry:
	for (i = 0; i < 8; i++)
		passwd[i] = ' ';
	for (i = 0; i < 8; i++) {
next:
		if ((c = ucase(rawin())) >= ' ')
			passwd[i] = c;
		if (c == '\r')
			break;
		if (c == 0x18)
			goto retry;
		if (c == 8) {
			if (i < 1)
				goto retry;
			passwd[--i] = ' ';
			goto next;
		}
		if (c == 3)
			quit();
	}
}

static VOID pagetest()
{
	if (cnt == 0)
		return;
	if (++tcnt >= cnt) {
		cputs("\r\n\r\nPress RETURN to Continue ");
		tcnt = rawin();
		conout('\r');
		if (tcnt == 3)
			quit();
		if (tcnt == 0x0f)
			eod = 1;
		tcnt = -1;
	}
}

static VOID typefile()
{
	struct { long addr; int len; } blk;
	register int	i, r, n;

	__bdos(BDOS_ERRMODE, (long) ERRMODE_RETURN);
	setdma(passwd);
	cur[6] |= 0x80;			/* f6': open read-only	*/
	cur[12] = 0;
	r = __bdos(BDOS_OPEN, (long) cur);
	if ((r & 0xff) == 0xff) {
		if (((r >> 8) & 0xff) == 7) {
			getpasswd();
			crlf();
			setdma(passwd);
			cur[6] |= 0x80;
			__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);
			r = __bdos(BDOS_OPEN, (long) cur);
		} else {
			cputs("\r\nERROR: Not typed: ");
			pfile(cur);
			conout(' ');
			error((r >> 8) & 0xff);
		}
	}
	if ((r & 0xff) == 0xff) {
		error(0);
		return;
	}
	__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);
	setdma(dirbuf);
	cur[32] = 0;
	eod = 0;
	while (!eod && __bdos(BDOS_READSEQ, (long) cur) == 0) {
		/* each run up to a line feed goes out as one block */
		for (i = 0; i < SECLEN && !eod; i += n) {
			for (n = 0; i + n < SECLEN; n++) {
				if (dirbuf[i + n] == 0x1a)
					eod = 1;
				if (eod || dirbuf[i + n] == '\n')
					break;
			}
			if (__bdos(BDOS_CONST, 0L) & 0xff) {
				rawin();
				quit();
			}
			if (n > 0) {
				blk.addr = (long) &dirbuf[i];
				blk.len = n;
				__bdos(BDOS_PRTBLK, (long) &blk);
			}
			if (!eod && i + n < SECLEN) {
				pagetest();
				conout('\n');
				n++;
			}
		}
	}
}

static VOID multifile()
{
	register int	i, k, r, nn;
	register char	*d;

	setdma(dirbuf);
	r = __bdos(BDOS_SFIRST, (long) fcb) & 0xff;
	if (r == 0xff)
		error(0);
	for (nn = 0; r != 0xff && nn < MAXNAMES; nn++) {
		d = &dirbuf[(r & 3) * 32 + 1];
		for (k = 0; k < 11; k++)
			names[nn][k] = d[k] & 0x7f;
		r = __bdos(BDOS_SNEXT, 0L) & 0xff;
	}
	for (i = 0; i < nn; i++) {
		for (k = 0; k < 36; k++)
			cur[k] = 0;
		cur[0] = fcb[0];
		for (k = 0; k < 11; k++)
			cur[k + 1] = names[i][k];
		for (k = 0; k < 8; k++)
			passwd[k] = fcb[16 + k];
		pagetest();
		crlf();
		pfile(cur);
		conout(':');
		pagetest();
		crlf();
		typefile();
	}
}

static VOID readtail()
{
	register int	i, n;

	tb[0] = ' ';
	n = _base->buff[0] & 0x7f;
	for (i = 0; i < n; i++)
		tb[i + 1] = _base->buff[i + 1];
	tb[n + 1] = 0;
	for (i = 1; tb[i] == ' ' || tb[i] == '\t'; i++)
		;
	if (tb[i] != 0)
		return;
	cputs("Enter file: ");
	tb[0] = 40;
	tb[1] = 0;
	__bdos(BDOS_RDCONBUF, (long) tb);
	n = tb[1] & 0xff;
	crlf();
	tb[0] = ' ';
	tb[1] = ' ';
	tb[n + 2] = 0;
	for (i = 2; i < n + 2; i++)
		tb[i] = ucase(tb[i]);
}

/*
 * type.plm's option scan: [PAGE], [NO PAGE] or [NOPAGE], PAGE by any
 * prefix.  Returns 1 to page, 0 not to, -1 when there is no option.
 */
static int options(p)
register char *p;
{
	register int	negate;

	negate = 0;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == 0)
		return (-1);
	if (*p != '[')
		erropt(0);
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == 'N') {
		p++;
		if (*p != 'O' && *p != ' ')
			erropt(1);
		p++;
		negate = 1;
		while (*p == ' ' || *p == '\t')
			p++;
	}
	if (*p != 'P')
		erropt(1);
	p++;
	if (*p == 'A') {
		p++;
		if (*p == 'G') {
			p++;
			if (*p == 'E')
				p++;
		}
	}
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != ']' && *p != 0)
		erropt(1);
	return (!negate);
}

int main()
{
	register int	i, r, paging, size;

	__bdos(BDOS_CONMODE, 1L);
	readtail();
	r = parse(tb, fcb);
	if (r < 0) {
		cputs("ERROR: Invalid file name. ");
		quit();
	}
	paging = (scbgetb(0x2c) == 0);
	if (r > 0 && (i = options(&tb[r])) >= 0)
		paging = i;
	if (paging) {
		size = scbgetb(0x1c);
		if (size < 5)		/* no page length set: v3's 24 */
			size = 24;
		cnt = size - 2;
	}
	for (i = 1; i <= 11 && fcb[i] != '?'; i++)
		;
	if (i <= 11)
		multifile();
	else {
		for (i = 0; i < 16; i++)
			cur[i] = fcb[i];
		for (i = 0; i < 8; i++)
			passwd[i] = fcb[16 + i];
		typefile();
	}
	quit();
	return (0);
}
