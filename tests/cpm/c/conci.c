/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * conci.c - Child workload for concurrent console and file state.
 */

#include "cpm.h"

static struct fcb	s;
static char		dbuf[SECLEN];


/*  ONE LINE, ONE BDOS CALL.  A switch happens at every BDOS call return
    (src/bdos/proc.c pdisp), so a line built out of cputs()/putdec()/cputs()
    is three switch points and the other process's output lands INSIDE it --
    which is the interleaving working, but it also splits `n=' from its digits
    and leaves nothing for a transcript to match on.  Everything this program
    asserts is therefore formatted into one buffer and emitted once.  */

static char	lnbuf[96];
static int	lnlen;

static VOID lnrst()
{
	lnlen = 0;
	lnbuf[0] = 0;
}

static VOID lnadd(s)
register char *s;
{
	while (*s != 0 && lnlen < (int)(sizeof lnbuf) - 1)
		lnbuf[lnlen++] = *s++;
	lnbuf[lnlen] = 0;
}

static VOID lndec(n)
unsigned n;
{
	char	tmp[8];
	register int i;

	i = 0;
	do {
		tmp[i++] = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0 && i < 7);
	while (i)
		if (lnlen < (int)(sizeof lnbuf) - 1)
			lnbuf[lnlen++] = tmp[--i];
		else
			i = 0;
	lnbuf[lnlen] = 0;
}

static VOID lnout()
{
	lnadd("\r\n");
	cputs(lnbuf);
	lnrst();
}

/*  Walk a whole directory; returns how many entries came back.  */

static int walk(pat)
char *pat;
{
	register int	rc, n;

	n = 0;
	mkfcb(pat, &s);
	setdma(dbuf);
	rc = __bdos(BDOS_SFIRST, (long) &s);
	while (rc != 255) {
		n += 1;
		setdma(dbuf);
		rc = __bdos(BDOS_SNEXT, (long) &s);
	}
	return (n);
}

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i;

	cputs("CONCI: I alive\r\n");

	for (i = 1; i <= 6; i++) {
		lnadd("  I-try-"); lndec((unsigned) i); lnout();
		if (i == 1) {
			walk("B:????????.???");	/* forces log_in(B:) */
			__bdos(BDOS_SELDSK, 0L);
		} else
			walk("????????.???");	/* reads every A: record */
		lnadd("  I-got-"); lndec((unsigned) i); lnout();
	}

	cputs("CONCI: I done\r\n");
	return (0);
}
