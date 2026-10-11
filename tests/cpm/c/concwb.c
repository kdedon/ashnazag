/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * concwb.c -- CONCWB.68K, the background half of the warm-boot ownership
 * test.  See src/cmd/concw.c for what the three programs prove.
 *
 * This one stands in for a running 8086 or Z80 interpreter: it holds a
 * pool segment that is NOT a parked process image -- pghld() is never set
 * on it -- and it is still using that segment after another process warm
 * boots.  It signs the segment so that the memory itself, and not only
 * the allocator's bookkeeping, is checked.
 */

#include "cpm.h"

#define	BDOS_FLAGWAIT	132
#define	BDOS_FLAGSET	133

#define	FLAG_BREADY	1		/* this program has its segment	*/
#define	FLAG_CDONE	2		/* CONCWC has taken what it can	*/

#define	SIGLEN		8

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

/*  The signature: eight bytes nothing else in the image spells, written
    at offset 0 of the segment and at the far end of it, so a page swap
    that moved only part of it would show.  */
static char sig[SIGLEN] = { 'B', 'G', 'S', 'E', 'G', '4', '2', '!' };

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i, seg, bad;
	register char	*p, *q;
	long		xa;

	seg = segget();
	if (seg == 0) {
		cputs("CONCWB: no segment\r\n");
		return (1);
	}
	xa = md.base;
	p = (char *) xa;
	q = (char *) (xa + 0xff00L);	/* a long: int is 16 bits here and
					   0xff00 as an int index would be
					   a negative offset		*/
	for (i = 0; i < SIGLEN; i++) {
		p[i] = sig[i];
		q[i] = sig[i];
	}

	cputs("CONCWB: B seg ");
	phex2(seg);
	cputs("\r\n");

	/*  Tell CONCW it may warm boot now.  */
	if (__bdos(BDOS_FLAGSET, (long) FLAG_BREADY) != 0) {
		cputs("CONCWB: flag set refused\r\n");
		return (1);
	}

	/*  Sleep through the warm boot and through CONCWC's grab.  A wait,
	    not a spin: this process must be out of the rotation so that the
	    CCP can read its next command line.  */
	if (__bdos(BDOS_FLAGWAIT, (long) FLAG_CDONE) != 0) {
		cputs("CONCWB: flag wait refused\r\n");
		return (1);
	}

	/*  And the whole point.  If the warm boot freed this segment the
	    descriptor went back to System-only and this read faults; if it
	    freed it AND CONCWC took it, the numbers CONCWC printed say so.
	    Either way the line below is the one the target requires.  */
	bad = 0;
	for (i = 0; i < SIGLEN; i++) {
		if (p[i] != sig[i])
			bad++;
		if (q[i] != sig[i])
			bad++;
	}
	cputs("CONCWB: B still seg ");
	phex2(seg);
	cputs(bad ? ", signature LOST\r\n" : ", signature intact\r\n");
	return (0);
}
