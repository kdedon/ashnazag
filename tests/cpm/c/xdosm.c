/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * xdosm.c - Exercise XDOS queues, flags, console assignment, and page
 * allocation in one process.
 */

#include "cpm.h"

#define	X_ABSRQ		128
#define	X_RELRQ		129
#define	X_MEMFR		130
#define	X_POLL		131
#define	X_FLGWT		132
#define	X_FLGSET	133
#define	X_MAKEQ		134
#define	X_OPENQ		135
#define	X_DELETQ	136
#define	X_READQ		137
#define	X_CREADQ	138
#define	X_WRITEQ	139
#define	X_CWRITEQ	140
#define	X_DELAY		141
#define	X_SETCON	148
#define	X_ASSIGN	149
#define	X_GETCON	153

#define	XFAIL	0x00ff

#define	QMSGLEN	8
#define	QDEPTH	4

struct xqmake {
	char	qm_name[8];
	short	qm_msglen;
	short	qm_ndep;
};

struct xqopen {
	char	qo_name[8];
	short	qo_id;
};

struct xqmsg {
	short	qx_id;
	char	qx_msg[16];
};

struct xmd {
	long	md_base;
	long	md_size;
};

struct xassign {
	char	xa_con;
	char	xa_name[8];
};

static int	nchk, nbad;

static char	mtext[] = "MESSAGE";

static VOID ck(ok, what)
int	ok;
char	*what;
{
	nchk++;
	if ( ! ok) {
		nbad++;
		cputs("XDOSM: FAIL ");
		cputs(what);
		cputs("\r\n");
	}
}

static VOID setname(d, s)
register char	*d, *s;
{
	register int	i;

	for (i = 0; i < 8; i++)
		d[i] = *s ? *s++ : ' ';
}

int main(argc, argv)
int	argc;
char	*argv[];
{
	struct xqmake	mk;
	struct xqopen	op;
	struct xqmsg	msg;
	struct xmd	md;
	struct xassign	as;
	int		i, k;
	long		base;

	cputs("XDOSM: XDOS calls, one process\r\n");

	/* ---- 153 / 148: the console this process owns ---- */

	ck(__bdos(X_GETCON, 0L) == 0, "153 did not return console 0");
	ck(__bdos(X_SETCON, 0L) == 0, "148 refused console 0");
	ck(__bdos(X_GETCON, 0L) == 0, "153 after 148 is not 0");
	ck(__bdos(X_SETCON, 5L) == XFAIL, "148 accepted console 5");
	ck(__bdos(X_GETCON, 0L) == 0, "a refused 148 changed the console");

	/* ---- 149: assign a NAMED process to a console ---- */
	/*  A process is named after its program.  */
	as.xa_con = 0;
	setname(as.xa_name, "XDOSM");
	ck(__bdos(X_ASSIGN, (long)&as) == 0, "149 did not find this process");
	setname(as.xa_name, "NOSUCH");
	ck(__bdos(X_ASSIGN, (long)&as) == XFAIL, "149 matched a name nothing has");
	setname(as.xa_name, "XDOSM");
	as.xa_con = 5;
	ck(__bdos(X_ASSIGN, (long)&as) == XFAIL, "149 accepted console 5");

	/* ---- 141: a zero delay is a return ---- */

	ck(__bdos(X_DELAY, 0L) == 0, "141 with 0 ticks did not return 0");

	/* ---- 131: the device numbers ---- */

	ck(__bdos(X_POLL, 1L) == 0, "131 says console output is not ready");
	ck(__bdos(X_POLL, 2L) == 0, "131 says the list device is not ready");
	ck(__bdos(X_POLL, 3L) == XFAIL, "131 accepted device 3");
	/*  DEVICE 0 IS NOT ASKED HERE.  Function 131 on the console
	    keyboard blocks until a key is there, and a scripted session
	    cannot promise one, so the call would hang or pass by luck.
	    Device 0 waits on the same pyield() spin as functions 132 and
	    137, which are measured above. */

	/* ---- 132/133: what the flags refuse ---- */

	ck(__bdos(X_FLGWT, 99L) == XFAIL, "132 accepted flag 99");
	ck(__bdos(X_FLGSET, 99L) == XFAIL, "133 accepted flag 99");
	ck(__bdos(X_FLGSET, 5L) == 0, "133 could not set flag 5");
	ck(__bdos(X_FLGSET, 5L) == XFAIL, "133 did not report a flag over run");
	ck(__bdos(X_FLGWT, 5L) == 0, "132 did not take a flag already set");
	/*  Nothing else is alive, so nobody can ever set flag 6: this is
	    the deviation xdos.c documents -- 0FFh rather than a hang.  */
	ck(__bdos(X_FLGWT, 6L) == XFAIL, "132 blocked with nobody able to set it");

	/* ---- 134-140: a queue, to and from one process ---- */

	setname(mk.qm_name, "XDOSMQ");
	mk.qm_msglen = QMSGLEN;
	mk.qm_ndep = QDEPTH;
	ck(__bdos(X_MAKEQ, (long)&mk) == 0, "134 would not make a queue");
	ck(__bdos(X_MAKEQ, (long)&mk) == 0, "134 refused a queue it already made");

	mk.qm_msglen = 999;
	ck(__bdos(X_MAKEQ, (long)&mk) == XFAIL, "134 accepted a 999-byte message");
	mk.qm_msglen = QMSGLEN;
	mk.qm_ndep = 999;
	ck(__bdos(X_MAKEQ, (long)&mk) == XFAIL, "134 accepted a 999-deep queue");
	mk.qm_ndep = QDEPTH;

	setname(op.qo_name, "XDOSMQ");
	op.qo_id = -1;
	ck(__bdos(X_OPENQ, (long)&op) == 0, "135 could not find the queue");
	ck(op.qo_id >= 0, "135 did not fill in a queue id");
	setname(op.qo_name, "NOTHERE");
	ck(__bdos(X_OPENQ, (long)&op) == XFAIL, "135 opened a queue nothing made");

	setname(op.qo_name, "XDOSMQ");
	__bdos(X_OPENQ, (long)&op);
	msg.qx_id = op.qo_id;

	/*  An empty queue: the conditional read is the whole point of
	    138 -- it says "nothing there" instead of blocking.  */
	ck(__bdos(X_CREADQ, (long)&msg) == XFAIL, "138 read an empty queue");

	setname(msg.qx_msg, mtext);
	ck(__bdos(X_WRITEQ, (long)&msg) == 0, "139 could not write");
	for (i = 0; i < 8; i++)
		msg.qx_msg[i] = 0;
	ck(__bdos(X_READQ, (long)&msg) == 0, "137 could not read it back");
	k = 1;
	for (i = 0; i < 7; i++)
		if (msg.qx_msg[i] != mtext[i])
			k = 0;
	ck(k, "137 did not return the message 139 wrote");

	/*  Fill it, then prove 140 refuses instead of blocking.  */
	setname(msg.qx_msg, "FILL");
	k = 1;
	for (i = 0; i < QDEPTH; i++)
		if (__bdos(X_CWRITEQ, (long)&msg) != 0)
			k = 0;
	ck(k, "140 could not fill the queue to its depth");
	ck(__bdos(X_CWRITEQ, (long)&msg) == XFAIL, "140 wrote past the queue's depth");
	k = 1;
	for (i = 0; i < QDEPTH; i++)
		if (__bdos(X_CREADQ, (long)&msg) != 0)
			k = 0;
	ck(k, "138 could not drain the queue");
	ck(__bdos(X_CREADQ, (long)&msg) == XFAIL, "138 read past the queue's end");

	setname(op.qo_name, "XDOSMQ");
	ck(__bdos(X_DELETQ, (long)&op) == 0, "136 could not delete the queue");
	ck(__bdos(X_OPENQ, (long)&op) == XFAIL, "135 found a deleted queue");
	ck(__bdos(X_DELETQ, (long)&op) == XFAIL, "136 deleted it twice");

	/* ---- 128/129/130: memory, a partition at a time ---- */
	md.md_base = 0;
	md.md_size = 0x1000000L;
	ck(__bdos(X_RELRQ, (long)&md) == XFAIL, "129 granted 16 MB");
	md.md_size = 0x10000L;
	ck(__bdos(X_RELRQ, (long)&md) == 0, "129 refused 64 KB");
	base = md.md_base;
	ck(base != 0 && md.md_size >= 0x10000L, "129 returned no memory");
	md.md_base = base;
	ck(__bdos(X_ABSRQ, (long)&md) == XFAIL, "128 granted memory in use");
	md.md_base = base;
	ck(__bdos(X_MEMFR, (long)&md) == 0, "130 would not free it");
	ck(__bdos(X_MEMFR, (long)&md) == XFAIL, "130 freed it twice");
	md.md_base = base;
	md.md_size = 0x10000L;
	ck(__bdos(X_ABSRQ, (long)&md) == 0 && md.md_base == base, "128 refused memory 130 gave back");
	ck(__bdos(X_MEMFR, (long)&md) == 0, "130 would not free the 128 memory");

	cputs("XDOSM: ");
	putdec(nchk);
	cputs(" checks, ");
	putdec(nbad);
	cputs(" failed\r\n");
	return (nbad != 0);
}
