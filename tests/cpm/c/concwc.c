/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * concwc.c -- CONCWC.68K, the claimant.  The CCP runs it after CONCW's
 * warm boot; see src/cmd/concw.c for the shape of the test.
 *
 * It prints the pool's size, then asks for every segment the pool will
 * part with and prints them.  The
 * list is the verdict on pgrelall(): CONCW's segment must be in it (a warm
 * boot still reclaims the warm-booting program's own scratch) and CONCWB's
 * must not (CONCWB is still alive and still using it).
 */

#include "cpm.h"

#define	BDOS_FLAGSET	133
#define	FLAG_CDONE	2

#define	MAXSEG		64		/* more than memory holds	*/

/* function 129: 64 KB of memory; its partition number, or 0 */
static struct { long base, len; } md;
static int segget()
{
	md.base = 0;
	md.len = 0x10000L;
	if (__bdos(129, (long) &md) != 0)
		return (0);
	return ((int) (md.base >> 20));
}

static char hexd[] = "0123456789ABCDEF";

static VOID phex2(v)
int v;
{
	conout(hexd[(v >> 4) & 0xf]);
	conout(hexd[v & 0xf]);
}

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i, n, seg;

	cputs("CONCWC: got");
	n = 0;
	for (i = 0; i < MAXSEG; i++) {
		seg = segget();
		if (seg == 0)
			break;
		conout(' ');
		phex2(seg);
		n++;
	}
	if (n == 0)
		cputs(" nothing");
	cputs("\r\n");

	/*  Release CONCWB, which has been waiting for exactly this.  The
	    segments taken above are not handed back: the warm boot at the
	    end of this program is what reclaims them, and that it does so
	    is the other half of what the target checks.  */
	__bdos(BDOS_FLAGSET, (long) FLAG_CDONE);
	return (0);
}
