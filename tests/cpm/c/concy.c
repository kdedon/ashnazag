/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * concy.c - CPU workload used by scheduler and console polling tests.
 */

#include "cpm.h"

/*  16 times CONCX's work.  */

#define	CONCYUNITS	16

static char	pad[4096];

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

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i;

	for (i = 0; i < sizeof pad; i++)
		pad[i] = (char)(i + 0x5b);

	work(CONCYUNITS);

	for (i = 0; i < sizeof pad; i++)
		if (pad[i] != (char)(i + 0x5b)) {
			printstr("CONCY: MEMORY CLOBBERED\r\n$");
			return (1);
		}
	printstr("CONCY: Y done, 4096 bytes of my own intact\r\n$");
	return (0);
}
