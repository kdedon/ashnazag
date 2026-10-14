/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * concq.c - Child workload for process creation and termination.
 */

#include "cpm.h"

/*  LONG ENOUGH TO OUTLIVE THREE THINGS, and the count is a measurement
    rather than a taste: the CCP's reload after CONCP exits (about 9 of
    these lines), the command typed at the prompt (about 7, one per
    character, because the CCP reads a character per BDOS call and each
    one is a switch point), and the LOAD of that command (about 35, same
    reason -- a program load is a few dozen BDOS calls and this program
    gets a whole turn, gap included, at every one).  That last number is
    the honest cost of round-robin scheduling against a partner that
    computes: whoever holds the machine holds it until it asks the BDOS
    for something, so a compute-heavy background job makes a foreground
    load feel slow.  Fixing that is preemption, which is the next stage.
    72 leaves better than a dozen lines of margin past the point CONCB
    starts printing.  */

#define	QLINES	72

static char	pad[4096];

/*  One line, one BDOS call -- src/cmd/conc.c says why.  */

static char lbuf[16];

static VOID qline(n)
int n;
{
	lbuf[0] = ' ';  lbuf[1] = ' ';  lbuf[2] = 'Q';  lbuf[3] = ' ';
	lbuf[4] = '0' + (n / 10) % 10;
	lbuf[5] = '0' + n % 10;
	lbuf[6] = '\r'; lbuf[7] = '\n'; lbuf[8] = '$'; lbuf[9] = 0;
	printstr(lbuf);
}

/*  The gap.  A pure computation with no BDOS call in it, which means no
    switch point: while this runs, this process HOLDS the machine and the
    CCP does not run.  That is still true and still cooperative -- taking
    the machine away from a program that is not asking is preemption, and
    preemption is the next stage.  What has changed is the other
    direction: the CCP waiting for a command no longer holds it.

    `spin' is volatile-by-accident (it is a static the compiler cannot
    prove nobody reads) so the loop is not optimised away.  */

static long	spin;

static VOID gap()
{
	register int	i, j;

	for (i = 0; i < 220; i++)
		for (j = 0; j < 200; j++)
			spin += (long)j;
}


int main(argc, argv)
int argc;
char *argv[];
{
	register int	i;

	for (i = 0; i < sizeof pad; i++)
		pad[i] = (char)(i + 0x27);

	printstr("CONCQ: Q alive\r\n$");

	for (i = 1; i <= QLINES; i++) {
		qline(i);
		gap();
	}

	for (i = 0; i < sizeof pad; i++)
		if (pad[i] != (char)(i + 0x27)) {
			printstr("CONCQ: MEMORY CLOBBERED\r\n$");
			return (1);
		}
	printstr("CONCQ: Q done, 4096 bytes of my own intact\r\n$");
	return (0);
}
