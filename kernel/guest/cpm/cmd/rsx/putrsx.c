/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * putrsx.c - PUT.RSX: console output copied into a file.  Functions 2,
 * 9 and 111 are recorded, and passed on too when echoing.  The file,
 * already made empty by PUT, is opened at the first full record;
 * function 60 sub-function 133 writes the last record and closes it,
 * 134 answers the FCB's address.
 */

extern int	rsxdown;
extern char	rsxwarm, rsxecho, rsxfcb[];
extern long	rsxbdos();

static char	buf[128];
static int	cnt, spent, opened, bad;
static char	scbpb[4] = { 0x3c, 0, 0, 0 };	/* get @CRDMA */

static void flushrec()
{
	long	dma;

	if (!opened) {
		opened = 1;
		rsxfcb[12] = rsxfcb[32] = 0;
		bad = (rsxbdos(15, (long) rsxfcb) & 0xff) == 0xff;
	}
	if (!bad) {
		dma = rsxbdos(49, (long) scbpb);
		rsxbdos(26, (long) buf);
		bad = rsxbdos(21, (long) rsxfcb) != 0;
		rsxbdos(26, dma);
	}
	cnt = 0;
}

static void put(c)
int c;
{
	buf[cnt++] = c;
	if (cnt == 128)
		flushrec();
}

/* the last record padded with ^Z; the close's answer */
static long finish()
{
	long	r;

	spent = 1;
	rsxwarm = 0xff;
	if (cnt) {
		while (cnt < 128)
			buf[cnt++] = 0x1a;
		flushrec();
	}
	if (!opened)
		return (0);
	r = rsxbdos(16, (long) rsxfcb) & 0xff;
	return (bad ? 0xff : r);
}

long rsx(fn, param)
int fn;
long param;
{
	register char	*p;
	register int	c, n;

	if (!spent)
		switch (fn) {
		case 60:
			c = *(char *) param & 0xff;
			if (c == 133)
				return (finish());
			if (c == 134)
				return ((long) rsxfcb);
			rsxdown = 1;
			return (0);
		case 2:
			put((int) param & 0xff);
			break;
		case 9:
			c = (int) rsxbdos(110, 0xffffL) & 0xff;
			for (p = (char *) param; *p != c; p++)
				put(*p);
			break;
		case 111:
			p = *(char **) param;
			n = *(unsigned *) (param + 4);
			while (n-- > 0)
				put(*p++);
			break;
		default:
			rsxdown = 1;
			return (0);
		}
	if (spent || rsxecho) {
		rsxdown = 1;
		return (0);
	}
	return (0);
}
