/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * u0t.c - Exercise read-only user-0 SYS-file fallback without exposing
 * ordinary files across user areas.
 */

#include "cpm.h"

#define	SHARED	"U0SHARE.TXT"		/* user 0, SYS			*/
#define	PLAIN	"U0PLAIN.TXT"		/* user 0, no SYS		*/
#define	OWN	"U3ONLY.TXT"		/* user area 3			*/
#define	FARREC	258			/* a record in the SECOND DIRECTORY ENTRY */

static struct fcb	f;
static char		buf[SECLEN];
static int		bad;

static VOID	puthex();
static VOID	putrec();
static int	openf();

int main(argc, argv)
int argc;
char *argv[];
{
	register int	r;

	setdma(buf);
	cputs("U0T: user ");
	putdec((unsigned) (__bdos(32, 0xffL) & 0xff));
	cputs("\r\n");

	/* ---- a user-0 SYS file opens from another user area ---- */
	r = openf(SHARED);
	cputs("U0T: open U0SHARE.TXT -> ");
	puthex(r);
	if (r == 0xff) {
		cputs("  BAD -- the fallback did not fire");
		bad++;
	}
	cputs("\r\n");

	if (r != 0xff) {
		/* record 0, then a record in the second extent */
		f.cur_rec = 0;
		cputs("U0T: rec 0 ");
		putrec(__bdos(BDOS_READSEQ, (long) &f) & 0xff);

		f.ran0 = 0;			/* CP/M-8000 keeps the	*/
		f.ran1 = (FARREC >> 8) & 0xff;	/* random record big-	*/
		f.ran2 = FARREC & 0xff;		/* endian (bdosrw.c:322) */
		cputs("U0T: rec 258 ");
		putrec(__bdos(BDOS_READRAN, (long) &f) & 0xff);

		/* ---- and it is read-only: error 3 through set$aret,
		   which is 03FFh, not a bare 3, and it prints the CP/M 3
		   long-form message, hence the "CP/M Error On" block in
		   the transcript				*/
		f.cur_rec = 0;
		buf[0] = 'X';
		__bdos(BDOS_ERRMODE, (long) ERRMODE_DISPRET);
		r = __bdos(BDOS_WRITESEQ, (long) &f) & 0xffff;
		__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);
		cputs("U0T: write -> ");
		puthex((r >> 8) & 0xff);
		puthex(r & 0xff);
		if (r != 0x03ff) {
			cputs("  BAD -- want 03FF, read-only file");
			bad++;
		}
		cputs("\r\n");
		__bdos(BDOS_CLOSE, (long) &f);
	}

	/* ---- a user-0 file WITHOUT SYS stays private to user 0 ---- */
	r = openf(PLAIN);
	cputs("U0T: open U0PLAIN.TXT -> ");
	puthex(r);
	if (r != 0xff) {
		cputs("  BAD -- a non-SYS file crossed the boundary");
		bad++;
	}
	cputs("\r\n");

	/* ---- the ordinary path still works, in the area we are in ---- */
	r = openf(OWN);
	cputs("U0T: open U3ONLY.TXT -> ");
	puthex(r);
	if (r == 0xff) {
		cputs("  BAD -- our own user area is unreachable");
		bad++;
	}
	cputs("\r\n");

	cputs("U0T: user ");
	putdec((unsigned) (__bdos(32, 0xffL) & 0xff));
	cputs(" after\r\n");

	cputs(bad ? "U0T: FAIL\r\n" : "U0T: PASS\r\n");
	return (bad != 0);
}


static int openf(name)
char *name;
{
	mkfcb(name, &f);
	setdma(buf);
	return (__bdos(BDOS_OPEN, (long) &f) & 0xff);
}


/*  A read's return code and the four bytes it landed in the buffer: the
    file is packed with its record number spelled out, so a record read
    from the wrong extent reads as the wrong number rather than as an
    error.  */

static VOID putrec(r)
register int r;
{
	register int	i;

	puthex(r);
	cputs(" [");
	for (i = 0; i < 4; i++)
		conout(buf[i] & 0x7f);
	cputs("]");
	if (r != 0) {
		cputs("  BAD -- read failed");
		bad++;
	}
	cputs("\r\n");
}


static VOID puthex(v)
register int v;
{
	static char	hex[] = "0123456789abcdef";

	conout(hex[(v >> 4) & 0x0f]);
	conout(hex[v & 0x0f]);
}
