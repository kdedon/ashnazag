/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * rsxldr.c - RSXLDR.68K: load Resident System Extensions.
 *
 *	RSXLDR name.RSX [T] [name.RSX [T]] ...
 *
 * Each module is read whole and handed to function 60 sub-function 127,
 * which relocates it below the chain.  `T' sets its warm-boot flag, so
 * it goes at the warm boot that ends this program.  Modules attach left
 * to right, the leftmost highest.
 */

#include "cpm.h"

#define	BDOS_CALLRSX	60
#define	RSX_ATTACH	127
#define	RSX_QUERY	126

/* offsets in the file: its 28-byte header, then the module's */
#define	F_HDR		28
#define	H_MAGIC		10
#define	H_PREV		16
#define	H_WARMFLG	20
#define	H_ENDCHAIN	30
#define	H_UP		32

struct rsxpb {
	char	rpfunc;
	char	rprsvd;
	long	rporg;
	long	rplen;
	long	rpsrc;
};

char		buf[16384];
struct fcb	fcb;
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

int attach(name, temp)
char *name;
int temp;
{
	register unsigned n;
	long		org;

	mkfcb(name, &fcb);
	if ((__bdos(BDOS_OPEN, (long) &fcb) & 0xff) == 255) {
		conputs("rsxldr: no such RSX file\r\n");
		return (1);
	}
	for (n = 0; n < sizeof buf; n += SECLEN) {
		setdma(&buf[n]);
		if (__bdos(BDOS_READSEQ, (long) &fcb) != 0)
			break;
	}
	__bdos(BDOS_CLOSE, (long) &fcb);
	setdma(_base->buff);
	if (n < F_HDR + 42 || buf[F_HDR + H_MAGIC] != 'R' || buf[F_HDR + H_MAGIC + 1] != 'S') {
		conputs("rsxldr: not an RSX\r\n");
		return (1);
	}
	if (temp)
		buf[F_HDR + H_WARMFLG] = 0xff;
	pb.rpfunc = RSX_ATTACH;
	pb.rprsvd = 0;
	pb.rporg = 0;
	pb.rplen = n;
	pb.rpsrc = (long) buf;
	org = __bdosl(BDOS_CALLRSX, (long) &pb);
	if (org < 256) {
		conputs("rsxldr: the system refused the attach ");
		puthex(org);
		conputs("\r\n");
		return (1);
	}
	/* through the module just linked */
	conputs("rsxldr: attached at ");
	puthex(org);
	conputs(temp ? " temporary\r\n" : " resident\r\n");
	return (0);
}

/* the links as attached, before the warm boot rewrites them */
VOID chain()
{
	register char	*p;
	long		m;

	pb.rpfunc = RSX_QUERY;
	pb.rprsvd = 0;
	for (m = __bdosl(BDOS_CALLRSX, (long) &pb); m != 0; m = *(long *) (p + H_UP)) {
		p = (char *) m;
		conputs("rsxldr: chain ");
		puthex(m);
		conputs(" prev=");
		puthex(*(long *) (p + H_PREV));
		conputs(" up=");
		puthex(*(long *) (p + H_UP));
		conputs(p[H_ENDCHAIN] ? " top\r\n" : "\r\n");
	}
}

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i, temp;

	if (argc < 2) {
		conputs("usage: RSXLDR name.RSX [T] ...\r\n");
		return (1);
	}
	for (i = 1; i < argc; i++) {
		temp = i + 1 < argc && (argv[i + 1][0] == 'T' || argv[i + 1][0] == 't') &&
		    argv[i + 1][1] == 0;
		if (attach(argv[i], temp) != 0)
			return (1);
		if (temp)
			i++;
	}
	chain();
	return (0);
}
