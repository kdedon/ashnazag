/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * concd.c - Exercise concurrent file I/O with a second process.
 */

#include "cpm.h"

#define	BDOS_CREATEPROC	144		/* XDOS create process	*/
#define	BDOS_MAKEQ	134
#define	BDOS_OPENQ	135
#define	BDOS_CREADQ	138

/* CONCE's last act: a message on this queue */
static struct { char name[8]; short len, dep; } mkq = { "CONCEQ  ", 2, 1 };
static struct { char name[8]; short id; } opq = { "CONCEQ  ", 0 };
static struct { short id; char msg[16]; } qm;

struct pcreq {
	struct fcb	pq_fcb;
	char		pq_tlen;
	char		pq_tail[128];
};

static struct pcreq	req;

static char *why[] = {
	"",
	"no such program",
	"not enough memory in the new page",
	"read error loading it",
	"program load error",
	"no free process descriptor",
	"no free 64 KB page -- this needs a 1 MB machine",
	"a second split-I/D program would need a second data bank"
};


/*  ONE LINE, ONE BDOS CALL.  A switch happens at every BDOS call return
    (src/bdos/proc.c pdisp), so a line built out of cputs()/putdec()/cputs()
    is three switch points and the other process's output lands INSIDE it --
    which is the interleaving working, but it also splits `n=' from its digits
    and leaves nothing for a transcript to match on.  Everything this program
    asserts is therefore formatted into one buffer and emitted once.  */

static char	lnbuf[96];
static int	lnlen;

static VOID lnrst()
{
	lnlen = 0;
	lnbuf[0] = 0;
}

static VOID lnadd(s)
register char *s;
{
	while (*s != 0 && lnlen < (int)(sizeof lnbuf) - 1)
		lnbuf[lnlen++] = *s++;
	lnbuf[lnlen] = 0;
}

static VOID lndec(n)
unsigned n;
{
	char	tmp[8];
	register int i;

	i = 0;
	do {
		tmp[i++] = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0 && i < 7);
	while (i)
		if (lnlen < (int)(sizeof lnbuf) - 1)
			lnbuf[lnlen++] = tmp[--i];
		else
			i = 0;
	lnbuf[lnlen] = 0;
}

static VOID lnout()
{
	lnadd("\r\n");
	cputs(lnbuf);
	lnrst();
}

/*  The walk.  One SEARCH_FIRST and then SEARCH_NEXT until 255, folding
    the twelve name bytes of every entry the BDOS delivers into a
    checksum.  The checksum is the point: a count alone would survive a
    swapped `pdirbuf' as long as the two scans happened to be the same
    length, and the names would not.

    setdma() is re-issued every turn on purpose.  `dmaadr' is a member of
    stvars, so re-setting it costs one more BDOS call -- one more switch
    point -- and asserts that the address this process set is the address
    this process's next entry arrives at.  */

static struct fcb	sfcb;
static char		dbuf[SECLEN];

static int		wcount;
static unsigned		wsum;

static VOID walk(pat)
char *pat;
{
	register int	rc, i;
	register char	*e;

	wcount = 0;
	wsum = 0;
	mkfcb(pat, &sfcb);
	setdma(dbuf);
	rc = __bdos(BDOS_SFIRST, (long) &sfcb);
	while (rc != 255) {
		e = &dbuf[(rc & 3) << 5];
		for (i = 0; i < 12; i++)
			wsum = (unsigned)((wsum << 1) ^ (wsum >> 15)
					  ^ (unsigned)(e[i] & 0x7f));
		wcount++;
		setdma(dbuf);
		rc = __bdos(BDOS_SNEXT, (long) &sfcb);
	}
}

/*  The named search.  Not a wildcard, so dhstart() builds a real filter
    and dhcand() skips entries on the strength of the signature table.  */

static struct fcb	nfcb;

static int named(name)
char *name;
{
	mkfcb(name, &nfcb);
	setdma(dbuf);
	return (__bdos(BDOS_SFIRST, (long) &nfcb) != 255);
}

static VOID report(tag)
char *tag;
{
	lnadd(tag);
	lndec((unsigned) wcount);
	lnadd(" s=");
	lndec(wsum);
	lnout();
}


int main(argc, argv)
int argc;
char *argv[];
{
	register int	k;
	int		n0;
	unsigned	s0;
	int		rounds, bad, nfail;


	cputs("CONCD: D start\r\n");

	/*  1.  Alone.  One process is live, nothing yields, and this is
	    the answer the whole test is measured against.  */
	walk("????????.???");
	n0 = wcount;
	s0 = wsum;
	report("CONCD: solo n=");

	if ( ! named("CONCE.68K")) {
		cputs("CONCD: FAIL -- CONCE.68K is not on the disk\r\n");
		return (1);
	}

	/*  2.  Beside CONCE.  */
	mkfcb("CONCE.68K", &req.pq_fcb);
	req.pq_tlen = 0;
	for (k = 0; k < 128; k++)
		req.pq_tail[k] = 0;

	if (__bdos(BDOS_MAKEQ, (long) &mkq) != 0 || __bdos(BDOS_OPENQ, (long) &opq) != 0) {
		cputs("CONCD: FAIL -- no queue\r\n");
		return (1);
	}
	qm.id = opq.id;
	k = __bdos(BDOS_CREATEPROC, (long) &req);
	if (k != 0) {
		cputs("CONCD: no second process: ");
		cputs(k > 0 && k < 8 ? why[k] : "refused");
		cputs("\r\n");
		return (1);
	}

	rounds = 0;
	bad = 0;
	nfail = 0;
	while (__bdos(BDOS_CREADQ, (long) &qm) != 0) {
		walk("????????.???");
		rounds++;
		if (wcount != n0 || wsum != s0) {
			bad++;
			if (bad == 1)
				report("CONCD: CLOBBERED n=");
		}
		if ( ! named("CONCE.68K"))
			nfail++;
	}

	lnadd("CONCD: rounds=");
	lndec((unsigned) rounds);
	lnadd(" clobbered=");
	lndec((unsigned) bad);
	lnadd(" lostname=");
	lndec((unsigned) nfail);
	lnout();
	report("CONCD: conc n=");

	if (rounds < 2) {
		cputs("CONCD: FAIL -- the child was gone before the second\r\n");
		cputs("       walk started, so nothing was concurrent\r\n");
		return (1);
	}
	if (bad) {
		cputs("CONCD: FAIL -- the directory walk gave a different\r\n");
		cputs("       answer while another process was walking\r\n");
		return (1);
	}
	if (nfail) {
		cputs("CONCD: FAIL -- a named search stopped finding a file\r\n");
		cputs("       that is on the disk\r\n");
		return (1);
	}
	cputs("CONCD: D done, walk stable under interleaving\r\n");
	return (0);
}
