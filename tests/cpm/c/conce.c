/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * conce.c - Child workload for concurrent file I/O.
 */

#include "cpm.h"

/*  Enough rounds to outlast a time slice, so CONCD walks beside them.  */

#define	EROUNDS	100

static struct { char name[8]; short id; } opq = { "CONCEQ  ", 0 };
static struct { short id; char msg[16]; } qm;
static struct fcb	sfcb;
static char		dbuf[SECLEN];

static int		wcount;
static unsigned		wsum;


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

static VOID walk(pat)
char *pat;
{
	register int	rc, i;
	register char	*e;

	wcount = 0;
	wsum = 0;
	mkfcb(pat, &sfcb);
	setdma(dbuf);
	rc = __bdos(BDOS_SFIRST, (long) &sfcb);
	while (rc != 255) {
		e = &dbuf[(rc & 3) << 5];
		for (i = 0; i < 12; i++)
			wsum = (unsigned)((wsum << 1) ^ (wsum >> 15)
					  ^ (unsigned)(e[i] & 0x7f));
		wcount++;
		setdma(dbuf);
		rc = __bdos(BDOS_SNEXT, (long) &sfcb);
	}
}


int main(argc, argv)
int argc;
char *argv[];
{
	register int	i;
	int		n0, bad;
	unsigned	s0;

	cputs("CONCE: E alive\r\n");

	walk("????????.68K");
	n0 = wcount;
	s0 = wsum;
	bad = 0;

	for (i = 1; i < EROUNDS; i++) {
		if (i == 2) {
			/*  The drive-B: login, in the middle of the run.  */
			mkfcb("B:????????.???", &sfcb);
			setdma(dbuf);
			__bdos(BDOS_SFIRST, (long) &sfcb);
			__bdos(BDOS_SELDSK, 0L);	/* back to A: */
		}
		walk("????????.68K");
		if (wcount != n0 || wsum != s0)
			bad++;
	}

	lnadd("CONCE: n=");
	lndec((unsigned) n0);
	lnadd(" s=");
	lndec(s0);
	lnadd(" wobble=");
	lndec((unsigned) bad);
	lnout();
	cputs("CONCE: E done\r\n");
	if (__bdos(135, (long) &opq) == 0) {
		qm.id = opq.id;
		__bdos(139, (long) &qm);
	}
	return (0);
}
