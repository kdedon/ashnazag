/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * conch.c - Exercise concurrent console and file state.
 */

#include "cpm.h"

#define	BDOS_CREATEPROC	144
#define	TARGET	"CONCLK.TXT"

struct pcreq { struct fcb pq_fcb; char pq_tlen; char pq_tail[128]; };
static struct pcreq	req;
static struct fcb	f;
static char		rec[SECLEN];

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i, k;

	cputs("CONCH: H start\r\n");

	mkfcb(TARGET, &f);
	__bdos(BDOS_DELETE, (long) &f);
	mkfcb(TARGET, &f);
	if (__bdos(BDOS_MAKE, (long) &f) == 255) {
		cputs("CONCH: FAIL -- cannot create the target\r\n");
		return (1);
	}
	for (i = 0; i < SECLEN; i++)
		rec[i] = 'h';
	setdma(rec);
	if (__bdos(BDOS_WRITESEQ, (long) &f) != 0) {
		cputs("CONCH: FAIL -- write error\r\n");
		return (1);
	}

	mkfcb("CONCI.68K", &req.pq_fcb);
	req.pq_tlen = 0;
	for (i = 0; i < 128; i++)
		req.pq_tail[i] = 0;
	k = __bdos(BDOS_CREATEPROC, (long) &req);
	if (k != 0) {
		cputs("CONCH: no second process\r\n");
		return (1);
	}

	cputs("CONCH: closing\r\n");
	__bdos(BDOS_CLOSE, (long) &f);
	cputs("CONCH: H done, the close returned\r\n");
	return (0);
}
