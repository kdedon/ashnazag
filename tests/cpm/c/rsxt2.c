/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * rsxt2.c - Inspect the resident chain and exercise console interception,
 * open post-processing, and delete refusal.  RSXT2 OFF flags every
 * module for removal.
 */

#include "cpm.h"

#define	BDOS_CALLRSX	60
#define	RSX_QUERY	126
#define	UCASE_COUNT	200	/* UCASE.RSX's sub-function		*/
#define	PROT_COUNT	201	/* PROT.RSX's sub-function		*/

/* module header offsets */
#define	H_SERIAL	0
#define	H_NEXT		12
#define	H_PREV		16
#define	H_WARMFLG	20
#define	H_NBANK		21
#define	H_NAME		22
#define	H_ENDCHAIN	30
#define	H_PATCH		31
#define	H_UP		32

#define	GUARD	"GUARD.TXT"	/* the file the delete test protects	*/

struct rsxpb {
	char	rpfunc;
	char	rprsvd;
	long	rporg;
	long	rplen;
	long	rpsrc;
};

struct rsxpb	pb;
struct fcb	fcb;

VOID puthex(n, nd)
long n;
int nd;
{
	register int	i, d;

	for (i = nd * 4 - 4; i >= 0; i -= 4) {
		d = (n >> i) & 0xf;
		conout(d < 10 ? '0' + d : 'A' + d - 10);
	}
}

long longat(p)
char *p;
{
	return (*(long *) p);
}

long rsxask(sub)
int sub;
{
	pb.rpfunc = sub;
	pb.rprsvd = 0;
	return (__bdosl(BDOS_CALLRSX, (long) &pb));
}

VOID report(what, sub)
char *what;
int sub;
{
	register int	r;

	r = rsxask(sub);
	conputs("rsxt2: ");
	conputs(what);
	if (r == 0xff)
		conputs(" unclaimed\r\n");
	else {
		conputs("=");
		puthex((long) r, 4);
		conputs("\r\n");
	}
}

/* from the newest module up to the oldest */
VOID chain()
{
	register char	*p;
	register int	i;
	long		m;

	if ((m = rsxask(RSX_QUERY)) == 0) {
		conputs("rsxt2: chain empty\r\n");
		return;
	}
	for (; m != 0; m = longat(p + H_UP)) {
		p = (char *) m;
		conputs("rsxt2: mod ");
		puthex(m, 8);
		conputs(" ");
		for (i = 0; i < 8; i++)
			conout(p[H_NAME + i]);
		conputs(" ser=");
		for (i = 0; i < 6; i++)
			puthex((long) p[H_SERIAL + i], 2);
		conputs(" wf="); puthex((long) p[H_WARMFLG], 2);
		conputs(" nb="); puthex((long) p[H_NBANK], 2);
		conputs(" ec="); puthex((long) p[H_ENDCHAIN], 2);
		conputs(" pl="); puthex((long) p[H_PATCH], 2);
		conputs(" prev="); puthex(longat(p + H_PREV), 8);
		conputs(" up="); puthex(longat(p + H_UP), 8);
		conputs("\r\n");
	}
}

VOID fileop(what, func, name)
char *what;
int func;
char *name;
{
	register int	r;

	mkfcb(name, &fcb);
	r = __bdos(func, (long) &fcb) & 0xff;
	conputs("rsxt2: ");
	conputs(what);
	conputs("=");
	putdec((unsigned) r);
	conputs(r == 255 ? " fail\r\n" : " ok\r\n");
}

/* every module flagged, so the warm boot removes them all */
VOID alloff()
{
	register char	*p;
	long		m;
	int		n;

	n = 0;
	for (m = rsxask(RSX_QUERY); m != 0; m = longat(p + H_UP)) {
		p = (char *) m;
		p[H_WARMFLG] = 0xff;
		n++;
	}
	conputs("rsxt2: flagged ");
	putdec((unsigned) n);
	conputs("\r\n");
}

int main(argc, argv)
int argc;
char *argv[];
{
	if (argc > 1 && argv[1][0] == 'O') {
		alloff();
		return (0);
	}
	chain();
	report("ucase", UCASE_COUNT);
	__bdos(BDOS_ERRMODE, (long) ERRMODE_RETURN);
	mkfcb(GUARD, &fcb);
	__bdos(BDOS_MAKE, (long) &fcb);
	__bdos(BDOS_CLOSE, (long) &fcb);
	__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);
	fileop("open guard", BDOS_OPEN, GUARD);
	fileop("open nosuch", BDOS_OPEN, "NOSUCH.XXX");
	fileop("delete guard", BDOS_DELETE, GUARD);
	fileop("reopen guard", BDOS_OPEN, GUARD);
	report("prot", PROT_COUNT);
	conputs("rsxt2: htpa=");
	puthex(_base->htpa, 8);
	conputs("\r\n");
	return (0);
}
