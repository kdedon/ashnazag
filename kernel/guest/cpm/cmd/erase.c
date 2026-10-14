/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * erase.c - ERASE, the CP/M 3 file eraser, for the C900.
 *
 *	ERASE			prompt for a name, then confirm each file
 *	ERASE d:name.typ[;pw]	erase the matching files
 *	ERASE *.* 		ask once before erasing every file
 *	ERASE names [CONFIRM]	ask before each file
 *	ERASE names [PASSWORD]	erase only the password XFCBs
 *
 * A protected file prompts for its password and is tried once more.
 */

#include "cpm.h"

#define	BDOS_GETUSER	32
#define	BDOS_PARSE	152

#define	MAXNAMES	1024

static char	fcb[36];
static char	sfcb[36];
static char	dirbuf[SECLEN];
static char	passwd[8];
static char	names[MAXNAMES][11];
static char	tb[SECLEN + 2];
static char	cname[11];		/* the file being erased	*/
static int	confirm, pwopt;

static VOID quit()
{
	__bdos(BDOS_WBOOT, 0L);
}

static VOID crlf()
{
	cputs("\r\n");
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
	conout(' ');
	if (code == 1)
		cputs("\r\nDisk I/O ");
	if (code == 2)
		cputs("\r\nDrive ");
	if (code == 3 || code == 2)
		cputs("Read Only");
	if (code == 5)
		cputs("Currently Opened");
	if (code == 7)
		cputs("Password Error");
	if (code < 3)
		quit();
}

/* returns 0 when erased, else the extended error code */
static int delete()
{
	register int	r;

	if (pwopt)
		fcb[5] |= 0x80;		/* f5': the XFCB only	*/
	setdma(passwd);
	r = __bdos(BDOS_DELETE, (long) fcb);
	fcb[5] &= 0x7f;
	if ((r & 0xff) != 0xff)
		return (0);
	r = (r >> 8) & 0xff;
	if (r == 1 || r == 2)
		error(r);
	return (r == 0 ? 0xff : r);
}

static VOID getpasswd()
{
	register int	i, c;

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

static VOID pname()
{
	register int	k;

	conout('A' + fcb[0] - 1);
	cputs(": ");
	for (k = 0; k < 11; k++) {
		if (k == 8)
			conout('.');
		conout(cname[k]);
	}
}

static VOID fileerr(code)
int code;
{
	if (!confirm) {
		pname();
		cputs("  ");
	}
	cputs("Not erased, ");
	error(code);
	crlf();
}

static VOID erase()
{
	register int	code;

	if ((code = delete()) != 0) {
		if (code == 7) {
			fileerr(code);
			getpasswd();
			crlf();
			code = delete();
		}
		if (code != 0)
			fileerr(code);
	}
}

/*
 * [PASSWORD] and [CONFIRM], each by any prefix, separated by blanks or
 * commas and closed by `]' or the end of the line.
 */
static VOID options(p)
register char *p;
{
	static char	*opt[] = { "PASSWORD", "CONFIRM" };
	register char	*q;
	register int	i, j, n;

	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (*p == ']' || *p == 0)
			return;
		for (n = 0; p[n] != 0 && p[n] != ' ' && p[n] != ','
		    && p[n] != ']' && p[n] != '['; n++)
			;
		for (i = 0; i < 2; i++) {
			q = opt[i];
			for (j = 0; j < n && q[j] == p[j]; j++)
				;
			if (n > 0 && j == n)
				break;
		}
		if (i == 2) {
			cputs("\r\nERROR: Missing Delimiter or");
			cputs("\r\n       Unrecognized Option Near: ");
			while (n-- > 0)
				conout(*p++);
			quit();
		}
		if (i == 0)
			pwopt = 1;
		else
			confirm = 1;
		p += n;
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
	confirm = 1;
	cputs("Enter filename: ");
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

int main()
{
	register int	i, k, r, nn;
	register char	*d;
	int		c;

	__bdos(BDOS_CONMODE, 1L);
	readtail();
	r = parse(tb, fcb);
	if (r < 0) {
		cputs("ERROR: Invalid file name ");
		quit();
	}
	if (r > 0) {
		while (tb[r] == ' ')
			r++;
		options(&tb[r + 1]);
	}
	if (fcb[0] == 0)
		fcb[0] = (__bdos(BDOS_CURDSK, 0L) & 0xff) + 1;
	__bdos(BDOS_ERRMODE, (long) ERRMODE_RETURN);
	for (i = 0; i < 8; i++)
		passwd[i] = fcb[16 + i];
	if (!confirm) {
		for (i = 1; i <= 11 && fcb[i] == '?'; i++)
			;
		if (i > 11 && !pwopt) {
			cputs("Confirm delete all user files (Y/N)?");
			c = __bdos(BDOS_CONIN, 0L) & 0xff;
			if (c != 'y' && c != 'Y')
				quit();
			crlf();
		}
	}

	/* collect the matches first: erasing disturbs the search */
	for (i = 0; i < 16; i++)
		sfcb[i] = fcb[i];
	setdma(dirbuf);
	r = __bdos(BDOS_SFIRST, (long) sfcb) & 0xff;
	if (r == 0xff) {
		cputs("No File ");
		quit();
	}
	for (nn = 0; r != 0xff && nn < MAXNAMES; nn++) {
		d = &dirbuf[(r & 3) * 32 + 1];
		for (k = 0; k < 11; k++)
			names[nn][k] = d[k] & 0x7f;
		r = __bdos(BDOS_SNEXT, 0L) & 0xff;
	}

	for (i = 0; i < nn; i++) {
		for (k = 0; k < 11; k++)
			fcb[k + 1] = cname[k] = names[i][k];
		fcb[12] = 0;
		if (confirm) {
			pname();
			cputs(" (Y/N)? ");
			c = __bdos(BDOS_CONIN, 0L) & 0xff;
			crlf();
			if (c == 3) {
				cputs("\r\n*** Aborted by ^C ***");
				quit();
			}
			if (c == 'y' || c == 'Y')
				erase();
		} else {
			if (__bdos(BDOS_CONST, 0L) & 0xff) {
				cputs("\r\n*** Aborted by ^C ***");
				quit();
			}
			erase();
		}
	}
	quit();
	return (0);
}
