/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * rsxt.c - Query an RSX and print console output through the resident
 * chain.
 */

#include "cpm.h"

#define	BDOS_CALLRSX	60
#define	RSX_COUNT	200	/* UCASE.RSX's own sub-function		*/

struct rsxpb {
	char	rpfunc;
	char	rprsvd;
	long	rporg;
	long	rplen;
	long	rpsrc;
};

struct rsxpb	pb;

VOID puthex(n)
long n;
{
	register int	i, d;

	for (i = 28; i >= 0; i -= 4) {
		d = (n >> i) & 0xf;
		conout(d < 10 ? '0' + d : 'A' + d - 10);
	}
}

int main()
{
	register int	r;

	pb.rpfunc = RSX_COUNT;
	r = __bdos(BDOS_CALLRSX, (long) &pb);
	if (r == 0xff)
		conputs("rsxt: fn60 unclaimed\r\n");
	else {
		conputs("rsxt: fn60 answered ");
		putdec((unsigned) r);
		conputs("\r\n");
	}
	pb.rpfunc = 126;			/* the system's chain query */
	conputs("rsxt: head=");
	puthex(__bdosl(BDOS_CALLRSX, (long) &pb));
	conputs("\r\n");
	conputs("rsxt: hello from rsxt\r\n");
	conputs("rsxt: htpa=");
	puthex(_base->htpa);
	conputs("\r\n");
	return (0);
}
