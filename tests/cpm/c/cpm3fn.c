/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*
 * cpm3fn.c - Exercise CP/M 3 functions 98 and 107-112. Function 98
 * must reclaim the allocation left by an unclosed file.
 */

#include "cpm.h"

static struct fcb	f;
static char		buf[SECLEN];
static long		freerec[1];
static char		serial[8];
static struct ccb	blk;

static long	freesp();
static int	bad;
static VOID	putlong();

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i;
	register int	r;
	long		free1;
	long		free2;
	long		free3;

	/* 107 - system serial number, six bytes to the given address */
	cputs("fn 107 serial  -> ");
	for (i = 0; i < 8; i++)
		serial[i] = 0;
	__bdos(BDOS_SERIAL, (long) serial);
	cputs(serial);

	/* 108 - program return code */
	__bdos(BDOS_RETCODE, 0x1234L);
	cputs("\r\nfn 108 set 1234, get -> ");
	putdec((unsigned) __bdos(BDOS_RETCODE, 0xffffL));
	bad += __bdos(BDOS_RETCODE, 0xffffL) != 0x1234;

	/* 109 - console mode */
	cputs("\r\nfn 109 mode was ");
	putdec((unsigned) __bdos(BDOS_CONMODE, 0xffffL));
	__bdos(BDOS_CONMODE, 2L);		/* no ^S/^Q */
	cputs(", set 2 -> ");
	putdec((unsigned) __bdos(BDOS_CONMODE, 0xffffL));
	bad += __bdos(BDOS_CONMODE, 0xffffL) != 2;
	__bdos(BDOS_CONMODE, 0L);
	cputs(", restored ");
	putdec((unsigned) __bdos(BDOS_CONMODE, 0xffffL));

	/* 110 - output delimiter: switch fn 9 from '$' to '#' and back */
	cputs("\r\nfn 110 delim was ");
	putdec((unsigned) __bdos(BDOS_OUTDELIM, 0xffffL));
	bad += __bdos(BDOS_OUTDELIM, 0xffffL) != '$';
	__bdos(BDOS_OUTDELIM, (long) '#');
	printstr("  fn 9 now stops at a hash#");
	__bdos(BDOS_OUTDELIM, (long) '$');
	printstr("  and at a dollar again$");

	/* 111/112 - print block to console and to the list device */
	blk.cbaddr = (long) "\r\nfn 111 printed this block";
	blk.cblen = 27;
	__bdos(BDOS_PRTBLK, (long) &blk);
	blk.cbaddr = (long) "fn 112 list block\r\n";
	blk.cblen = 19;
	r = __bdos(BDOS_LSTBLK, (long) &blk);
	cputs("\r\nfn 112 (to list device) returned ");
	putdec((unsigned) r);

	/* 98 - free blocks: host files hold no blocks in an FCB, so it only succeeds */
	free1 = freesp();
	cputs("\r\nfree records before ");
	putlong(free1);
	mkfcb("FN98.TMP", &f);
	__bdos(BDOS_DELETE, (long) &f);
	mkfcb("FN98.TMP", &f);
	if ((__bdos(BDOS_MAKE, (long) &f) & 0xff) == 0xff) {
		cputs("\r\ncpm3fn: cannot create FN98.TMP\r\n");
		return (1);
	}
	setdma(buf);
	for (i = 0; i < 8; i++)
		if (__bdos(BDOS_WRITESEQ, (long) &f) != 0) {
			cputs("\r\ncpm3fn: write failed\r\n");
			return (1);
		}
	free2 = freesp();		/* the file is never closed */
	cputs(", with a block allocated ");
	putlong(free2);
	bad += __bdos(BDOS_FREEBLK, 0L) != 0;
	free3 = freesp();
	cputs(", after fn 98 ");
	putlong(free3);
	mkfcb("FN98.TMP", &f);
	__bdos(BDOS_DELETE, (long) &f);
	cputs(bad ? "\r\nCPM3FN: FAIL\r\n" : "\r\nCPM3FN: PASS\r\n");
	return (bad != 0);
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
