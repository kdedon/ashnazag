/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * trunct.c - Truncate a 300-record file to 100 records, verify its contents
 * and reclaimed blocks, and reject invalid requests.
 */

#include "cpm.h"

#define	NREC	300
#define	KEEP	100		/* truncate to records 0..KEEP-1	*/

static struct fcb	f;
static char		buf[SECLEN];
static long		freerec[1];

static int		bad;

static long	freesp();
static long	fsize();
static VOID	setran();
static VOID	putlong();

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i;
	int		r;
	long		free0, free1, free2;
	long		sz;

	free0 = freesp();

	/* ---- lay down NREC records, each stamped with its number ---- */
	mkfcb("TRUNCT.TXT", &f);
	__bdos(BDOS_DELETE, (long) &f);
	mkfcb("TRUNCT.TXT", &f);
	setdma(buf);
	if ((__bdos(BDOS_MAKE, (long) &f) & 0xff) == 0xff) {
		cputs("TRUNCT: BAD -- cannot create TRUNCT.TXT\r\n");
		return (1);
	}
	for (i = 0; i < NREC; i++) {
		buf[0] = (char) (i & 0xff);
		buf[1] = (char) ((i >> 8) & 0xff);
		if (__bdos(BDOS_WRITESEQ, (long) &f) & 0xff) {
			cputs("TRUNCT: BAD -- write failed at record ");
			putdec((unsigned) i);
			cputs("\r\n");
			return (1);
		}
	}
	__bdos(BDOS_CLOSE, (long) &f);
	free1 = freesp();

	sz = fsize();
	cputs("TRUNCT: wrote ");
	putlong(sz);
	cputs(" records, free ");
	putlong(free0);
	cputs(" -> ");
	putlong(free1);
	cputs("\r\n");
	if (sz != (long) NREC) {
		cputs("TRUNCT: BAD -- file size after writing\r\n");
		bad++;
	}

	/* ---- truncate to KEEP records ---- */
	mkfcb("TRUNCT.TXT", &f);
	setran((long) (KEEP - 1));
	i = __bdos(99, (long) &f) & 0xff;
	cputs("TRUNCT: fn 99 -> ");
	putdec((unsigned) i);
	if (i == 0xff) {
		cputs("  BAD -- truncate refused\r\n");
		return (1);
	}
	sz = fsize();
	free2 = freesp();
	cputs("  size ");
	putlong(sz);
	cputs("  free ");
	putlong(free2);
	cputs("\r\n");
	if (sz != (long) KEEP) {
		cputs("TRUNCT: BAD -- size after truncate\r\n");
		bad++;
	}


	/* ---- the surviving data is still the data ---- */
	mkfcb("TRUNCT.TXT", &f);
	setdma(buf);
	if ((__bdos(BDOS_OPEN, (long) &f) & 0xff) == 0xff) {
		cputs("TRUNCT: BAD -- cannot reopen\r\n");
		return (1);
	}
	for (i = 0; i < KEEP; i++) {
		buf[0] = buf[1] = 0;
		if (__bdos(BDOS_READSEQ, (long) &f) & 0xff) {
			cputs("TRUNCT: BAD -- read failed at record ");
			putdec((unsigned) i);
			cputs("\r\n");
			bad++;
			break;
		}
		if ((buf[0] & 0xff) != (i & 0xff)
		    || (buf[1] & 0xff) != ((i >> 8) & 0xff)) {
			cputs("TRUNCT: BAD -- record ");
			putdec((unsigned) i);
			cputs(" is not itself\r\n");
			bad++;
			break;
		}
	}
	if (i == KEEP) {
		if ((__bdos(BDOS_READSEQ, (long) &f) & 0xff) == 0)
			cputs("TRUNCT: BAD -- record beyond the cut still readable\r\n"),
			bad++;
		else
			cputs("TRUNCT: 100 records verified, EOF at 100\r\n");
	}
	__bdos(BDOS_CLOSE, (long) &f);

	/* ---- refuse a truncate that would extend ---- */
	mkfcb("TRUNCT.TXT", &f);
	setran((long) (KEEP + 10));
	i = __bdos(99, (long) &f) & 0xff;
	cputs("TRUNCT: fn 99 past the end -> ");
	putdec((unsigned) i);
	if (i != 0xff) {
		cputs("  BAD -- must refuse");
		bad++;
	}
	cputs("\r\n");

	/* ---- refuse a wildcard, and refuse it as error 9 ----
	   v3 puts check$wild first in func99 (bdos30.asm:4799) and
	   check$wild reports through set$aret (:1774, :4373-4380): the
	   caller gets 09FFh, not a bare 0FFh, and the console gets
	   "? in Filename".  Error mode 0FEh asks for both halves in one
	   call -- the message AND the code -- so the transcript proves
	   the message and this proves the code.		*/
	__bdos(BDOS_ERRMODE, (long) ERRMODE_DISPRET);
	mkfcb("TRUNC?.TXT", &f);
	setran((long) 0);
	r = __bdos(99, (long) &f);
	__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);
	cputs("TRUNCT: fn 99 on a wildcard -> ");
	putdec((unsigned) ((r >> 8) & 0xff));
	cputs("/");
	putdec((unsigned) (r & 0xff));
	if (((r >> 8) & 0xff) != 9 || (r & 0xff) != 0xff) {
		cputs("  BAD -- want error 9, return 255");
		bad++;
	}
	cputs("\r\n");

	cputs(bad ? "TRUNCT: FAIL\r\n" : "TRUNCT: PASS\r\n");
	return (bad != 0);
}


static VOID setran(r)
long r;
{
	f.ran0 = (char) ((r >> 16) & 0xff);
	f.ran1 = (char) ((r >> 8) & 0xff);
	f.ran2 = (char) (r & 0xff);
}


static long fsize()
{
	mkfcb("TRUNCT.TXT", &f);
	__bdos(35, (long) &f);
	return (((long) (f.ran0 & 0xff) << 16) | ((long) (f.ran1 & 0xff) << 8)
		| (long) (f.ran2 & 0xff));
}


/* function 46: free records, a long at the DMA address */
static long freesp()
{
	freerec[0] = 0;
	setdma((char *) freerec);
	__bdos(46, 0L);
	setdma(buf);
	return (freerec[0]);
}


static VOID putlong(n)
long n;
{
	if (n >= 10L)
		putlong(n / 10L);
	conout((int) ('0' + n % 10L));
}
