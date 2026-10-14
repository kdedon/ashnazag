/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * xdose.c - Child workload for XDOS queue, event-flag, and termination
 * tests.
 */

#include "cpm.h"

#define	X_FLGSET	133
#define	X_OPENQ		135
#define	X_CREADQ	138
#define	X_WRITEQ	139
#define	X_TERM		143

#define	XFLAG	3

/*  The counting loop's ceiling, so that a delay which never returns
    still ends the session with a transcript.  */
#define	ELIMIT	400

struct xqopen {
	char	qo_name[8];
	short	qo_id;
};

struct xqmsg {
	short	qx_id;
	char	qx_msg[16];
};

static struct xqopen	op;
static struct xqmsg	msg;

static VOID setname(d, s)
register char	*d, *s;
{
	register int	i;

	for (i = 0; i < 8; i++)
		d[i] = *s ? *s++ : ' ';
}

static int openq(name)
char	*name;
{
	setname(op.qo_name, name);
	op.qo_id = -1;
	if (__bdos(X_OPENQ, (long)&op) != 0)
		return (-1);
	return (op.qo_id);
}

static int n;

/*  One line, ONE BDOS call: cputs() is function 111, so the unit of
    interleaving is a line rather than a character.  */

static VOID eline()
{
	char	b[16];
	register char	*p;
	register int	d, seen;

	p = b;
	*p++ = ' '; *p++ = ' '; *p++ = 'E'; *p++ = ' ';
	seen = 0;
	for (d = 100; d > 0; d /= 10) {
		if (n / d != 0 || seen || d == 1) {
			*p++ = '0' + (n / d) % 10;
			seen = 1;
		}
	}
	*p++ = '\r'; *p++ = '\n'; *p = 0;
	n++;
	cputs(b);
}

int main(argc, argv)
int	argc;
char	*argv[];
{
	register int	i;
	int		qmsg, qstop;

	n = 1;
	cputs("XDOSE: alive\r\n");

	if ((qmsg = openq("XDOSQ")) < 0 || (qstop = openq("XDOSS")) < 0) {
		cputs("XDOSE: FAIL could not open the queues\r\n");
		return (1);
	}

	/*  Run 1: while D is inside function 141.  */
	msg.qx_id = qstop;
	for (i = 0; i < ELIMIT; i++) {
		if (__bdos(X_CREADQ, (long)&msg) == 0)
			break;		/* D says it has woken up */
		eline();
	}
	cputs("XDOSE: stopped\r\n");

	/*  Run 2: while D is inside function 132.  */
	for (i = 0; i < 5; i++)
		eline();
	if (__bdos(X_FLGSET, (long)XFLAG) != 0)
		cputs("XDOSE: FAIL 133 refused the flag\r\n");
	cputs("XDOSE: set\r\n");

	/*  Run 3: while D is inside function 137.  */
	for (i = 0; i < 5; i++)
		eline();
	msg.qx_id = qmsg;
	setname(msg.qx_msg, "HELLO");
	if (__bdos(X_WRITEQ, (long)&msg) != 0)
		cputs("XDOSE: FAIL 139 refused the message\r\n");
	cputs("XDOSE: wrote\r\n");

	/*  MP/M's Terminate.  It does not return, so the line after it
	    is a statement that it did.  */
	__bdos(X_TERM, 0L);
	cputs("XDOSE: FAIL 143 came back\r\n");
	return (1);
}
