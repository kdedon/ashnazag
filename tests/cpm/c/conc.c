/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * conc.c - Run a CPU workload alongside a second process to exercise
 * scheduling.
 */

#include "cpm.h"

#define	BDOS_CREATEPROC	144		/* XDOS create process	*/


/*  A OUTLIVES B, and the count had to grow when the tick started
    preempting.  Six was enough while the only switch point was a
    BDOS call: A and B alternated line for line, so B's three lines and
    its 4 KB self-check were done by A's second or third.  Under
    preemption B does that 4 KB WHILE A prints -- the machine is shared
    by time rather than by call, so B's silent work no longer waits for
    A to stop asking for the console -- and at six lines A finished
    first, which is the one thing this program must not do: the claim
    "once B ended, A ran alone" needs an A that is still there.  Twelve
    is that with margin, and it is not a timing constant in any sharper
    sense than six was.  */

#define	ALINES	12

/*  The function 144 parameter block (src/bdos/proc.c struct pcreq): the
    program's FCB, unopened, and the command tail it should see in its
    base page.  */

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

/*  One line, ONE BDOS call.  That matters here and nowhere else in this
    tree: the dispatcher runs at the BDOS call boundary, so a line printed
    with cputs() -- function 2 per character -- interleaves with the other
    process CHARACTER by character, which is a louder proof and an
    unreadable transcript.  Function 9 makes the unit of interleaving a
    line, which is what the verify target greps for.  */

static char lbuf[64];

static VOID pline(tag, n)
char *tag;
int n;
{
	register char	*p;
	register int	d, seen;

	for (p = lbuf; *tag != 0; )
		*p++ = *tag++;
	seen = 0;
	for (d = 10000; d > 0; d /= 10) {
		if (n / d != 0 || seen || d == 1) {
			*p++ = '0' + (n / d) % 10;
			seen = 1;
		}
	}
	*p++ = '\r';
	*p++ = '\n';
	*p++ = '$';
	printstr(lbuf);
}


int main(argc, argv)
int argc;
char *argv[];
{
	register int	i, k;

	printstr("CONC A: two processes\r\n$");

	mkfcb("CONCB.68K", &req.pq_fcb);
	req.pq_tlen = 0;
	for (i = 0; i < 128; i++)
		req.pq_tail[i] = 0;

	k = __bdos(BDOS_CREATEPROC, (long) &req);
	if (k != 0) {
		cputs("CONC: no second process: ");
		cputs(k > 0 && k < 8 ? why[k] : "refused");
		cputs("\r\n");
		return (1);
	}



	for (i = 1; i <= ALINES; i++)
		pline("  A ", i);

	printstr("CONC: A done\r\n$");
	return (0);
}
