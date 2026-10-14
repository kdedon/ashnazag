/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * lockt.c - Two processes contend for one record (functions 42/43).
 *
 * LOCKT runs as A and starts itself again as B.  Flags hand the turn over:
 *
 *   A: lock r0, spawn B       -- flag 0 -->  B: r0 locked? writes refused?
 *                                            lock r1, then wait
 *   A: r1 locked? unlock r0   -- flag 1 -->  B: lock r0, close
 *   A: r0 free after close?   <-- flag 2 --  B: reopen, lock r1
 *   A: r1 free after B died?  <-- flag 3 --  B: exit
 *
 * Each result is one line "X name=code"; the verify target checks them.
 */

#include "cpm.h"

#define	BDOS_FLAGWAIT	132
#define	BDOS_FLAGSET	133
#define	BDOS_CREATEPROC	144
#define	BDOS_LOCK	42
#define	BDOS_UNLOCK	43
#define	TARGET		"LOCKT.DAT"

struct pcreq { struct fcb pq_fcb; char pq_tlen; char pq_tail[128]; };
static struct pcreq	req;
static struct fcb	f;
static char		rec[SECLEN];
static char		lnbuf[64];
static char		who;

/* One line per BDOS call, so the other process cannot split it. */
static VOID say(name, n)
char *name;
unsigned n;
{
	register char	*p;
	char		tmp[6];
	register int	i;

	p = lnbuf;
	*p++ = who;
	*p++ = ' ';
	while (*name)
		*p++ = *name++;
	*p++ = '=';
	i = 0;
	do {
		tmp[i++] = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0);
	while (i)
		*p++ = tmp[--i];
	*p++ = '\r';
	*p++ = '\n';
	*p = 0;
	cputs(lnbuf);
}

static unsigned call(fn, r)
int fn, r;
{
	f.ran0 = 0;
	f.ran1 = 0;
	f.ran2 = (char) r;
	setdma(rec);
	return (__bdos(fn, (long) &f) & 0xffff);
}

static VOID reopen()
{
	mkfcb(TARGET, &f);
	say("open", __bdos(BDOS_OPEN, (long) &f) & 0xff);
}

static int parent()
{
	register int	i;
	register unsigned rc;

	mkfcb(TARGET, &f);
	__bdos(BDOS_DELETE, (long) &f);
	mkfcb(TARGET, &f);
	__bdos(BDOS_MAKE, (long) &f);
	for (i = 0; i < SECLEN; i++)
		rec[i] = 'a';
	call(BDOS_WRITERAN, 0);
	call(BDOS_WRITERAN, 1);
	__bdos(BDOS_CLOSE, (long) &f);
	reopen();

	say("lock0", call(BDOS_LOCK, 0));
	say("lock5", call(BDOS_LOCK, 5));	/* never written */

	mkfcb("LOCKT.68K", &req.pq_fcb);
	req.pq_tlen = 2;
	req.pq_tail[0] = ' ';
	req.pq_tail[1] = 'B';
	if (__bdos(BDOS_CREATEPROC, (long) &req) != 0) {
		cputs("A: no second process\r\n");
		return (1);
	}
	__bdos(BDOS_FLAGWAIT, 0L);
	say("lock1", call(BDOS_LOCK, 1));	/* B holds it */
	say("unlock0", call(BDOS_UNLOCK, 0));
	__bdos(BDOS_FLAGSET, 1L);
	__bdos(BDOS_FLAGWAIT, 2L);
	say("relock0", call(BDOS_LOCK, 0));	/* B closed its file */
	say("unlock0", call(BDOS_UNLOCK, 0));
	__bdos(BDOS_FLAGWAIT, 3L);

	/* B exits holding r1; its locks go with it. */
	for (i = 0; i < 2000; i++)
		if ((rc = call(BDOS_LOCK, 1)) != 8)
			break;
	say("relock1", rc);
	say("write1", call(BDOS_WRITERAN, 1));
	__bdos(BDOS_CLOSE, (long) &f);
	cputs("A: done\r\n");
	return (0);
}

static int child()
{
	reopen();
	say("lock0", call(BDOS_LOCK, 0));	/* A holds it */
	say("write0", call(BDOS_WRITERAN, 0));
	say("read0", call(BDOS_READRAN, 0));
	say("lock1", call(BDOS_LOCK, 1));
	__bdos(BDOS_FLAGSET, 0L);
	__bdos(BDOS_FLAGWAIT, 1L);
	say("lock0", call(BDOS_LOCK, 0));	/* A let it go */
	__bdos(BDOS_CLOSE, (long) &f);
	__bdos(BDOS_FLAGSET, 2L);
	reopen();
	say("lock1", call(BDOS_LOCK, 1));
	__bdos(BDOS_FLAGSET, 3L);
	cputs("B: exits holding r1\r\n");
	return (0);
}

int main(argc, argv)
int argc;
char *argv[];
{
	who = argc > 1 ? 'B' : 'A';
	return (who == 'B' ? child() : parent());
}
