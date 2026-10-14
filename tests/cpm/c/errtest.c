/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * errtest.c - Exercise silent and display-and-return BDOS error modes, then
 * restore normal file I/O.
 */

#include "cpm.h"

static struct fcb	f;
static char		buf[SECLEN];

static VOID	report();
static int	bad;
static VOID	errmode();

int main(argc, argv)
int argc;
char *argv[];
{
	register int	r;
	register int	mode;
	register int	pass;

	for (pass = 0; pass < 2; pass++) {
		mode = pass == 0 ? ERRMODE_RETURN : ERRMODE_DISPRET;
		cputs(pass == 0 ? "\r\n-- mode 0FFH (silent return) --\r\n"
				: "\r\n-- mode 0FEH (display and return) --\r\n");
		errmode(mode);

		/* 4: a drive that is not there */
		mkfcb("K:NOSUCH.TXT", &f);
		report("open on K:", __bdos(BDOS_OPEN, (long) &f), 4);

		/* 2: the disk marked read-only under us */
		mkfcb("ERRT.TMP", &f);
		__bdos(BDOS_DELETE, (long) &f);
		mkfcb("ERRT.TMP", &f);
		if ((__bdos(BDOS_MAKE, (long) &f) & 0xff) == 0xff) {
			cputs("errtest: cannot create ERRT.TMP\r\n");
			errmode(ERRMODE_DEFAULT);
			return (1);
		}
		setdma(buf);
		__bdos(28, 0L);			/* A: read-only */
		report("write to R/O disk",
			__bdos(BDOS_WRITESEQ, (long) &f), 2);
		__bdos(37, 1L);			/* reset drive A: */

		/* 3: a file marked read-only */
		mkfcb("ERRT.TMP", &f);
		f.ftype[0] |= 0x80;		/* t1' = read-only */
		__bdos(30, (long) &f);		/* set file attributes */
		mkfcb("ERRT.TMP", &f);
		__bdos(BDOS_OPEN, (long) &f);
		setdma(buf);
		report("write to R/O file",
			__bdos(BDOS_WRITESEQ, (long) &f), 3);

		/* put the file back and clean up while errors still return */
		mkfcb("ERRT.TMP", &f);
		f.ftype[0] &= 0x7f;
		__bdos(30, (long) &f);
		mkfcb("ERRT.TMP", &f);
		__bdos(BDOS_DELETE, (long) &f);
	}

	errmode(ERRMODE_DEFAULT);
	cputs("\r\n-- back in the default mode --\r\n");

	/* the BDOS must still be usable: a normal open of a real file */
	mkfcb("HELLO.TXT", &f);
	setdma(buf);
	if ((__bdos(BDOS_OPEN, (long) &f) & 0xff) == 0xff)
		cputs("ERRTEST: HELLO.TXT open FAILED\r\n");
	else if (__bdos(BDOS_READSEQ, (long) &f) != 0)
		cputs("ERRTEST: HELLO.TXT read FAILED\r\n");
	else
		cputs("ERRTEST: normal file I/O still works\r\n");
	__bdos(BDOS_CLOSE, (long) &f);
	cputs(bad ? "ERRTEST: FAIL\r\n" : "ERRTEST: PASS\r\n");
	return (bad != 0);
}


static VOID errmode(mode)
int mode;
{
	__bdos(BDOS_ERRMODE, (long) mode);
}


/* print "<what>: rc=hhll code=n" for a BDOS return value, want the code expected */
static VOID report(what, rc, want)
char *what;
int rc, want;
{
	if ((rc & 0xff) != 0xff || ((rc >> 8) & 0xff) != want)
		bad++;
	cputs(what);
	cputs(": low=");
	putdec((unsigned) (rc & 0xff));
	cputs(" code=");
	putdec((unsigned) ((rc >> 8) & 0xff));
	cputs((rc & 0xff) == 0xff ? " (error returned)\r\n"
				  : " (NO ERROR RETURNED)\r\n");
}
