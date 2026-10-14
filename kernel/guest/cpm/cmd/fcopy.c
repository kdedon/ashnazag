/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * fcopy.c - copy a file record-by-record through the BDOS sequential
 * file calls: open/make/delete/read/write/close plus SETDMA, all via
 * the SC #2 shim.  Usage:  FCOPY SRC.TYP DST.TYP
 *
 * The copy is built in a scratch file of type $$$ and only put in the
 * destination's place once it has been written, closed, and the close
 * CHECKED.  Deleting the destination first -- which this did -- threw a
 * good file away before knowing a replacement could be produced: a full
 * disk, a refused directory write or an exhausted directory then left the
 * user with a partial file, or none at all, where a whole one had been.
 */

#include "cpm.h"

static struct fcb	src;
static struct fcb	dst;
static struct fcb	tmp;
static char		rnbuf[36];	/* function 23 wants two FCBs	*/
static char		buf[SECLEN];

/*  Give up, leaving nothing of the half-made copy behind.  The
    destination has not been touched at this point, so the user still has
    the file that was there.  */

static int giveup(msg)
char *msg;
{
	cputs("fcopy: ");
	cputs(msg);
	cputs("\r\n");
	__bdos(BDOS_DELETE, (long) &tmp);
	return (1);
}

int main(argc, argv)
int argc;
char *argv[];
{
	register unsigned	n;
	register int		i;

	if (argc != 3) {
		cputs("usage: fcopy src dst\r\n");
		return (1);
	}
	mkfcb(argv[1], &src);
	mkfcb(argv[2], &dst);

	if ((__bdos(BDOS_OPEN, (long) &src) & 0xff) == 0xff) {
		cputs("fcopy: cannot open ");
		cputs(argv[1]);
		cputs("\r\n");
		return (1);
	}

	/*  The scratch file is the destination's name with type $$$, which
	    is what PIP uses, so a run that is cut off leaves a file whose
	    name says what it is.  A destination of that type would BE the
	    scratch file, and the guarantee this program now makes -- the
	    old file is still there if the copy does not finish -- cannot be
	    kept for it, so it is refused rather than quietly broken.  */
	mkfcb(argv[2], &tmp);
	if (tmp.ftype[0] == '$' && tmp.ftype[1] == '$' && tmp.ftype[2] == '$') {
		cputs("fcopy: a destination of type $$$ is the scratch file's\r\n");
		cputs("       own name; copy to another name and rename it\r\n");
		return (1);
	}
	tmp.ftype[0] = '$';
	tmp.ftype[1] = '$';
	tmp.ftype[2] = '$';
	__bdos(BDOS_DELETE, (long) &tmp);	/* a stale one from before */
	mkfcb(argv[2], &tmp);			/* delete scrambles the FCB */
	tmp.ftype[0] = '$';
	tmp.ftype[1] = '$';
	tmp.ftype[2] = '$';
	if ((__bdos(BDOS_MAKE, (long) &tmp) & 0xff) == 0xff) {
		cputs("fcopy: no directory space for the scratch file\r\n");
		return (1);
	}

	setdma(buf);
	n = 0;
	while (__bdos(BDOS_READSEQ, (long) &src) == 0) {
		if (__bdos(BDOS_WRITESEQ, (long) &tmp) != 0)
			return (giveup("write error (disk full?)"));
		n++;
	}
	if ((__bdos(BDOS_CLOSE, (long) &tmp) & 0xff) == 0xff)
		return (giveup("close failed"));
	__bdos(BDOS_CLOSE, (long) &src);
	setdma(_base->buff);		/* directory work off the default DMA */

	/*  Only now is there something worth replacing the destination
	    with.  CP/M cannot rename onto a name that exists, so the old
	    file goes first; a delete that finds nothing is not an error
	    here, but the rename is the operation that must be checked --
	    past it the old file is gone and the scratch holds the only
	    copy, so it is left in place to be recovered.  */
	__bdos(BDOS_DELETE, (long) &dst);
	mkfcb(argv[2], &dst);			/* delete scrambles the FCB */
	for (i = 0; i < 16; i++) {
		rnbuf[i] = ((char *) &tmp)[i];
		rnbuf[16 + i] = ((char *) &dst)[i];
	}
	if ((__bdos(BDOS_RENAME, (long) rnbuf) & 0xff) == 0xff) {
		cputs("fcopy: cannot rename the scratch file over ");
		cputs(argv[2]);
		cputs("\r\n       the copy is in the .$$$ file\r\n");
		return (1);
	}

	cputs("fcopy: copied ");
	putdec(n);
	cputs(" records\r\n");
	return (0);
}
