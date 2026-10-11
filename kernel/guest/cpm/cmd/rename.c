/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * rename.c - RENAME, the CP/M 3 file renamer, for the C900.
 *
 *	RENAME			prompt for the new name, then the old
 *	RENAME new=old[;pw]	rename one file
 *	RENAME n?w.*=o?d.*	rename every match; each `?' in the old
 *				name must be a `?' in the new one
 *
 * An existing new name asks before it is deleted; a protected file asks
 * for its password.
 */

#include "cpm.h"

#define	BDOS_PARSE	152

#define	MAXNAMES	1024

static char	newf[36];		/* the new name			*/
static char	curf[36];		/* the old name as typed	*/
static char	oldf[36];		/* one match of it		*/
static char	dirbuf[SECLEN];
static char	passwd[8];
static char	names[MAXNAMES][11];
static char	tb[SECLEN + 2];

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

static VOID failed()
{
	cputs("\r\nERROR: Not renamed, ");
}

static VOID error(code)
int code;
{
	if (code == 0) {
		cputs("ERROR:  No such file to rename.");
		quit();
	}
	if (code == 1) {
		cputs("\r\nDisk I/O.");
		quit();
	}
	if (code == 2) {
		cputs("\r\nERROR: Drive read only.");
		quit();
	}
	if (code == 3)
		cputs("read only.");
	if (code == 5)
		cputs("Currently Opened.");
	if (code == 7)
		cputs("Bad password.");
	if (code == 8)
		cputs("file already exists");
	if (code == 9) {
		cputs("Invalid wildcard.");
		quit();
	}
}

static VOID pfile(f)
register char *f;
{
	register int	k;

	for (k = 1; k <= 11; k++) {
		if (k == 9)
			conout('.');
		conout(f[k] & 0x7f);
	}
}

/* returns 0 when renamed, else the extended error code */
static int rename(f)
register char *f;
{
	register int	i, r;

	for (i = 0; i < 16; i++)
		f[16 + i] = newf[i];
	setdma(passwd);
	__bdos(BDOS_ERRMODE, (long) ERRMODE_RETURN);
	r = __bdos(BDOS_RENAME, (long) f);
	__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);
	if ((r & 0xff) != 0xff)
		return (0);
	r = (r >> 8) & 0xff;
	if (r < 3)
		error(r);
	return (r);
}

static VOID getpasswd()
{
	register int	i, c;

	crlf();
	cputs("Enter password: ");
retry:
	for (i = 0; i < 8; i++)
		passwd[i] = ' ';
	for (i = 0; i < 8; i++) {
next:
		if ((c = ucase(rawin())) >= ' ')
			passwd[i] = c;
		if (c == '\r') {
			crlf();
			break;
		}
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

static int wildcard()
{
	register int	i, wild;

	wild = 0;
	for (i = 1; i <= 11; i++)
		if (curf[i] == '?') {
			if (newf[i] != '?') {
				failed();
				cputs("Invalid wildcard.");
				quit();
			}
			wild = 1;
		}
	return (wild);
}

static VOID fileerr(f, code)
char *f;
int code;
{
	failed();
	pfile(f);
	conout(' ');
	error(code);
}

/* rename each match of curf, collected first: renaming moves the search */
static VOID singlefile()
{
	register int	i, k, r, nn, code;
	register char	*d;
	char		save[12];

	setdma(dirbuf);
	r = __bdos(BDOS_SFIRST, (long) curf) & 0xff;
	if (r == 0xff)
		error(0);
	for (nn = 0; r != 0xff && nn < MAXNAMES; nn++) {
		d = &dirbuf[(r & 3) * 32 + 1];
		for (k = 0; k < 11; k++)
			names[nn][k] = d[k] & 0x7f;
		r = __bdos(BDOS_SNEXT, 0L) & 0xff;
	}
	for (k = 0; k < 12; k++)
		save[k] = newf[k];
	for (i = 0; i < nn; i++) {
		for (k = 0; k < 36; k++)
			oldf[k] = 0;
		oldf[0] = curf[0];
		for (k = 1; k <= 11; k++) {
			oldf[k] = names[i][k - 1];
			newf[k] = (curf[k] == '?') ? oldf[k] : save[k];
		}
		if ((code = rename(oldf)) == 8) {
			fileerr(newf, code);
			cputs(", delete (Y/N)?");
			if (ucase(__bdos(BDOS_CONIN, 0L) & 0xff) != 'Y')
				continue;
			setdma(passwd);
			__bdos(BDOS_DELETE, (long) newf);
			code = rename(oldf);
		}
		if (code == 7) {
			fileerr(oldf, code);
			getpasswd();
			code = rename(oldf);
		}
		if (code != 0)
			fileerr(oldf, code);
		else {
			crlf();
			pfile(newf);
			conout('=');
			pfile(oldf);
		}
	}
}

static VOID badentry()
{
	failed();
	cputs("ERROR:  Invalid File.\r\n");
	quit();
}

/* read one name at the prompt into tb at p; returns its length */
static int askname(p)
register char *p;
{
	register int	i, n;

	p[0] = 40;
	p[1] = 0;
	__bdos(BDOS_RDCONBUF, (long) p);
	n = p[1] & 0xff;
	for (i = 2; i < n + 2; i++)
		p[i] = ucase(p[i]);
	return (n);
}

int main()
{
	register int	i, r, n;

	n = _base->buff[0] & 0x7f;
	for (i = 0; i < n; i++)
		tb[i] = _base->buff[i + 1];
	tb[n] = 0;
	for (i = 0; tb[i] == ' ' || tb[i] == '\t'; i++)
		;
	if (tb[i] == 0) {
		/* build "new=old" from two prompts */
		cputs("Enter New Name: ");
		if ((n = askname(tb)) <= 0) {
			cputs("\r\nERROR: Incorrect file specification.\r\n");
			quit();
		}
		tb[0] = ' ';
		tb[1] = ' ';
		crlf();
		cputs("Enter Old Name: ");
		r = askname(&tb[n + 2]);
		crlf();
		tb[n + 2] = '=';
		tb[n + 3] = ' ';
		tb[n + r + 4] = 0;
	}
	r = parse(tb, newf);
	if (r <= 0 || parse(&tb[r + 1], curf) < 0)
		badentry();
	for (i = 0; i < 8; i++)
		passwd[i] = curf[16 + i];
	if (newf[0] != 0) {
		if (curf[0] != 0) {
			if (newf[0] != curf[0])
				badentry();
		} else
			curf[0] = newf[0];
	}
	newf[0] = curf[0];		/* an existing new name is deleted here */
	if (wildcard())
		singlefile();
	else {
		for (i = 16; i < 36; i++)
			curf[i] = 0;
		if (rename(curf) != 0)
			singlefile();
	}
	quit();
	return (0);
}
