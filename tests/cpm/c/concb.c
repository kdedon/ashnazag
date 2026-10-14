/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * concb.c - Competing workload for the concurrency exercisers.
 */

#include "cpm.h"

#define	BLINES	3

static char	pad[4096];

/*  One line, one BDOS call -- src/cmd/conc.c says why.  */

static char lbuf[16];

static VOID pline(n)
int n;
{
	lbuf[0] = ' ';  lbuf[1] = ' ';  lbuf[2] = 'B';  lbuf[3] = ' ';
	lbuf[4] = '0' + (n / 10) % 10;
	lbuf[5] = '0' + n % 10;
	lbuf[6] = '\r'; lbuf[7] = '\n'; lbuf[8] = '$'; lbuf[9] = 0;
	printstr(lbuf);
}


int main(argc, argv)
int argc;
char *argv[];
{
	register int	i;

	for (i = 0; i < sizeof pad; i++)
		pad[i] = (char)(i + 0x5b);

	printstr("CONCB: B alive\r\n$");

	for (i = 1; i <= BLINES; i++)
		pline(i);

	for (i = 0; i < sizeof pad; i++)
		if (pad[i] != (char)(i + 0x5b)) {
			printstr("CONCB: MEMORY CLOBBERED\r\n$");
			return (1);
		}
	printstr("CONCB: B done, 4096 bytes of my own intact\r\n$");
	return (0);
}
