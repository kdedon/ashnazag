/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * beep.c - Sound the console bell n times (default 1) through BDOS function
 * 2.
 */

#include "cpm.h"

int main(argc, argv)
int argc;
char *argv[];
{
	register int	n;
	register char	*p;

	n = 0;
	if (argc > 1)
		for (p = argv[1]; *p >= '0' && *p <= '9'; p++)
			n = n * 10 + (*p - '0');
	if (n <= 0)
		n = 1;
	cputs("BEEP ");
	putdec((unsigned) n);
	cputs("\r\n");
	while (n-- > 0)
		conout('\007');
	cputs("done\r\n");
	return (0);
}
