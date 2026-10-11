/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * getrsx.c - GET.RSX: console input from a file.  Functions 1, 6, 10 and
 * 11 are served from the file until it ends; then every call passes on
 * and the module goes at the next warm boot.  Function 60 sub-function
 * 129 stops it, 130 answers its FCB's address.
 */

extern int	rsxdown;
extern char	rsxwarm, rsxecho, rsxfcb[];
extern long	rsxbdos();

static char	buf[128];
static int	pos, cnt, spent, opened, last;
static char	scbpb[4] = { 0x3c, 0, 0, 0 };	/* get @CRDMA */

static void stop()
{
	spent = 1;
	rsxwarm = 0xff;
}

/* the next byte of the file, or -1 at its end */
static int gnext()
{
	register int	i;
	long		dma, r;

	if (spent)
		return (-1);
	if (pos >= cnt) {
		if (!opened) {
			opened = 1;
			rsxfcb[12] = rsxfcb[32] = 0;
			if ((rsxbdos(15, (long) rsxfcb) & 0xff) == 0xff) {
				stop();
				return (-1);
			}
		}
		/* the caller's DMA stays its own */
		dma = rsxbdos(49, (long) scbpb);
		rsxbdos(26, (long) buf);
		r = rsxbdos(20, (long) rsxfcb);
		rsxbdos(26, dma);
		if (r != 0) {
			stop();
			return (-1);
		}
		pos = 0;
		cnt = 128;
	}
	i = buf[pos++] & 0xff;
	if (i == 0x1a) {
		stop();
		return (-1);
	}
	return (i);
}

/* a byte with CR LF and a lone LF as CR, or -1 */
static int getch()
{
	register int	c;

	if ((c = gnext()) == '\n' && last == '\r')
		c = gnext();
	last = c;
	return (c == '\n' ? '\r' : c);
}

static void echo(c)
int c;
{
	if (rsxecho)
		rsxbdos(2, (long) c);
}

/* function 10: buf[0] the size, buf[1] the count, then the line */
static long readline(p)
register char *p;
{
	register int	c, n, max;

	max = p[0] & 0xff;
	for (n = 0; (c = getch()) >= 0 && c != '\r'; ) {
		echo(c);
		if (n < max)
			p[2 + n++] = c;
	}
	if (c == '\r')
		echo('\r');
	else if (n == 0) {
		rsxdown = 1;
		return (0);
	}
	p[1] = n;
	return (0);
}

long rsx(fn, param)
int fn;
long param;
{
	register int	c;

	if (fn == 60 && !spent) {
		c = *(char *) param & 0xff;
		if (c == 129) {
			stop();
			return (0);
		}
		if (c == 130)
			return ((long) rsxfcb);
	}
	if (!spent)
		switch (fn) {
		case 1:
			if ((c = getch()) < 0)
				break;
			echo(c);
			return (c);
		case 6:
			c = param & 0xff;
			if (c == 0xfe)
				return (0xff);
			if ((c == 0xff || c == 0xfd) && (c = getch()) >= 0)
				return (c);
			break;
		case 10:
			if (param != 0)
				return (readline((char *) param));
			break;
		case 11:
			return (0xff);
		}
	rsxdown = 1;
	return (0);
}
