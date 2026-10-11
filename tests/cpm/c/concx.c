/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * concx.c - Exercise process scheduling with a competing workload.
 */

#include "cpm.h"

#define	BDOS_CREATEPROC	144		/* XDOS create process	*/

/*  The unit of computation, shared with concy.c by being the same code
    with a different repeat count.  220*200 long adds is concq.c's `gap':
    about a million instructions, and a tick lands roughly every 15,500,
    so one unit is scores of ticks.  */

#define	CONCXUNITS	1

static long	spin;

static VOID work(units)
int units;
{
	register int	u, i, j;

	for (u = 0; u < units; u++)
		for (i = 0; i < 220; i++)
			for (j = 0; j < 200; j++)
				spin += (long)j;
}

struct pcreq {
	struct fcb	pq_fcb;
	char		pq_tlen;
	char		pq_tail[128];
};

static struct pcreq	req;

static char *why[] = {
	"",
	"no such program",
	"not enough memory in the new page",
	"read error loading it",
	"program load error",
	"no free process descriptor",
	"no free 64 KB page -- this needs a 1 MB machine",
	"a second split-I/D program would need a second data bank"
};

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i, k;

	printstr("CONCX: X start\r\n$");

	mkfcb("CONCY.68K", &req.pq_fcb);
	req.pq_tlen = 0;
	for (i = 0; i < 128; i++)
		req.pq_tail[i] = 0;

	k = __bdos(BDOS_CREATEPROC, (long) &req);
	if (k != 0) {
		cputs("CONCX: no second process: ");
		cputs(k > 0 && k < 8 ? why[k] : "refused");
		cputs("\r\n");
		return (1);
	}

	/*  From here to the print below there is no BDOS call at all.  */
	work(CONCXUNITS);

	printstr("CONCX: X done\r\n$");
	return (0);
}
