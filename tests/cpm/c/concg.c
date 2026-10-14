/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * concg.c - Child workload for concurrent file operations.
 */

#include "cpm.h"

static struct fcb	s;
static char		dbuf[SECLEN];

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i;

	cputs("CONCG: G alive\r\n");

	for (i = 1; i <= 12; i++) {
		cputs("  G ");
		putdec((unsigned) i);
		cputs("\r\n");
	}

	/*  The first reference to drive B: -- log_in() (src/bdos/fileio.c),
	    which takes the file-system lock, rebuilds B:'s allocation
	    vector, and calls dhopen()/dhdone() (src/bdos/dskhash.c) on the
	    one signature table the machine has.  */
	mkfcb("B:????????.???", &s);
	setdma(dbuf);
	__bdos(BDOS_SFIRST, (long) &s);
	__bdos(BDOS_SELDSK, 0L);

	cputs("CONCG: FLIPPED\r\n");

	for (i = 1; i <= 8; i++)
		cputs("  G .\r\n");

	cputs("CONCG: G done\r\n");
	return (0);
}
