/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * concw.c -- memory a program got from function 129 is freed by its warm
 * boot; another process's is not.
 *
 *   CONCW   gets memory, starts CONCWB, waits on flag 1, exits.
 *   CONCWB  gets memory, signs it, sets flag 1, waits on flag 2, checks it.
 *   CONCWC  (next at the prompt) takes all it can get and lists it, sets flag 2.
 *
 * CONCW's memory must be in CONCWC's list, CONCWB's must not.
 */

#include "cpm.h"

#define	BDOS_CREATEPROC	144
#define	BDOS_FLAGWAIT	132
#define	BDOS_FLAGSET	133

#define	FLAG_BREADY	1		/* CONCWB has its segment	*/

struct pcreq {
	struct fcb	pq_fcb;
	char		pq_tlen;
	char		pq_tail[128];
};

static struct pcreq	req;
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
	register int	i, seg;

	seg = segget();
	if (seg == 0) {
		cputs("CONCW: no segment -- this test needs more than 512 KB\r\n");
		return (1);
	}
	cputs("CONCW: A seg ");
	phex2(seg);
	cputs("\r\n");

	mkfcb("CONCWB.68K", &req.pq_fcb);
	req.pq_tlen = 0;
	for (i = 0; i < 128; i++)
		req.pq_tail[i] = 0;
	if (__bdos(BDOS_CREATEPROC, (long) &req) != 0) {
		cputs("CONCW: no second process\r\n");
		return (1);
	}

	/*  Not a delay: CONCWB sets this the instant it owns its segment,
	    so the warm boot below cannot happen first.  */
	if (__bdos(BDOS_FLAGWAIT, (long) FLAG_BREADY) != 0) {
		cputs("CONCW: flag wait refused\r\n");
		return (1);
	}

	cputs("CONCW: A exits, warm boot follows\r\n");
	return (0);		/* the warm boot, and pgrelall() with it */
}
