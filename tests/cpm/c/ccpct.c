/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * ccpct.c - Stress the CCP reload after a program ends.
 *   CCPCT F  fill the TPA from the end of bss to just below the stack
 */

#include "cpm.h"

int main(argc, argv)
int argc;
char *argv[];
{
	register char	*p, *e;
	register int	i;
	char		here;

	if (argc > 1 && argv[1][0] == 'F') {
		p = (char *) (_base->lbss + _base->bsslen);
		e = &here - 64;
		for (i = 0; p < e; i++)
			*p++ = (char) (0xE5 ^ i);
		cputs("CCPCT: filled\r\n");
		return (0);
	}
	cputs("CCPCT: F\r\n");
	return (1);
}
