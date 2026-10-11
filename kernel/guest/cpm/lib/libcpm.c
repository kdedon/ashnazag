/*
 * libcpm.c -- startup and console helpers for CP/M-68K C programs.
 */

#include "cpm.h"

struct bpage	*_base;

extern int	main();

#define	NARGV	16

static char	tail[SECLEN + 1];
static char	*argv[NARGV + 1];

int conout(c)
int c;
{
	return (__bdos(BDOS_CONOUT, (long) c));
}

int conin()
{
	return (__bdos(BDOS_CONIN, 0L));
}

/* one function 111 call for the whole string */
VOID cputs(s)
register char *s;
{
	register char	*p;
	struct ccb	blk;

	for (p = s; *p != 0; p++)
		;
	blk.cbaddr = (long) s;
	blk.cblen = (unsigned) (p - s);
	__bdos(BDOS_PRTBLK, (long) &blk);
}

/* through function 2, which an RSX sees */
VOID conputs(s)
register char *s;
{
	while (*s != 0)
		conout(*s++);
}

VOID printstr(s)
char *s;
{
	__bdos(BDOS_PRINTSTR, (long) s);
}

VOID putdec(n)
unsigned n;
{
	if (n >= 10)
		putdec(n / 10);
	conout('0' + n % 10);
}

VOID setdma(p)
char *p;
{
	__bdos(BDOS_SETDMA, (long) p);
}

/* "NAME", "NAME.EXT" or "D:NAME.EXT", upper case, into a fresh FCB */
VOID mkfcb(name, f)
register char *name;
struct fcb *f;
{
	register char	*p;
	register int	i;

	p = (char *) f;
	for (i = 0; i < sizeof (struct fcb); i++)
		*p++ = 0;
	if (name[0] != 0 && name[1] == ':') {
		f->drvcode = name[0] - 'A' + 1;
		name += 2;
	}
	for (i = 0; i < 8; i++)
		f->fname[i] = ' ';
	for (i = 0; i < 3; i++)
		f->ftype[i] = ' ';
	for (i = 0; i < 8 && *name != 0 && *name != '.'; i++)
		f->fname[i] = *name++;
	while (*name != 0 && *name != '.')
		name++;
	if (*name == '.')
		name++;
	for (i = 0; i < 3 && *name != 0; i++)
		f->ftype[i] = *name++;
}

/* main's status becomes the program return code (function 108); 0xffff would read it */
VOID _setrc(status)
int status;
{
	if (status == -1)
		status = 1;
	__bdos(BDOS_RETCODE, (long) (unsigned) status);
}

int _cstart(bp)
struct bpage *bp;
{
	register char	*p;
	register int	argc, n, i;
	int		rc;

	_base = bp;
	n = bp->buff[0] & 0x7f;
	for (i = 0; i < n; i++)
		tail[i] = bp->buff[i + 1];
	tail[n] = 0;
	argv[0] = "";
	argc = 1;
	p = tail;
	while (argc < NARGV) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == 0)
			break;
		argv[argc++] = p;
		while (*p != 0 && *p != ' ' && *p != '\t')
			p++;
		if (*p != 0)
			*p++ = 0;
	}
	argv[argc] = 0;
	rc = main(argc, argv);
	_setrc(rc);
	return (rc);
}

/* the compiler's own calls for structure copies */
char *memcpy(d, s, n)
char *d, *s;
unsigned long n;
{
	register char	*p = d;

	while (n--)
		*p++ = *s++;
	return (d);
}

char *memmove(d, s, n)
char *d, *s;
unsigned long n;
{
	register char	*p = d;

	if (d < s)
		return (memcpy(d, s, n));
	p += n;
	s += n;
	while (n--)
		*--p = *--s;
	return (d);
}

char *memset(d, c, n)
char *d;
int c;
unsigned long n;
{
	register char	*p = d;

	while (n--)
		*p++ = c;
	return (d);
}
