/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * concp.c - Exercise process creation and termination.
 */

#include "cpm.h"

#define	BDOS_CREATEPROC	144		/* XDOS create process	*/

/*  The function 144 parameter block (src/bdos/proc.c struct pcreq).  */

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

	printstr("CONCP: P start\r\n$");

	mkfcb("CONCQ.68K", &req.pq_fcb);
	req.pq_tlen = 0;
	for (i = 0; i < 128; i++)
		req.pq_tail[i] = 0;

	k = __bdos(BDOS_CREATEPROC, (long) &req);
	if (k != 0) {
		cputs("CONCP: no second process: ");
		cputs(k > 0 && k < 8 ? why[k] : "refused");
		cputs("\r\n");
		return (1);
	}

	/*  And that is all.  The exit below is a warm boot: this process
	    ends, the CCP is reloaded into the page CONCP was using, and
	    CONCQ keeps its own page and its turn in the ready list.  */
	printstr("CONCP: P done\r\n$");
	return (0);
}
